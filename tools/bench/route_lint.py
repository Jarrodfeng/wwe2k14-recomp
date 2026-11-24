r"""H2 / L1 ROUTE / FLAG LINT (S103: failed runs kept coming from forgotten switches; runs 24 / 25 / 26).

  py -I tools\bench\route_lint.py                       lint every route under tools\input_scripts: exit 1 on an ERROR
  py -I tools\bench\route_lint.py --route R [--final "FLAGS"] [--exe EXE] [--extra "FLAGS"]
                                                        one route; --final = the run's whole final flag line (run_list.ps1 passes it: the route's header flags + what run_list added)
  py -I tools\bench\route_lint.py --plain               also list the plain routes (no flags, no intent) whose steps the per-run lint refuses fast-forwarded (S109 FF)
  py -I tools\bench\route_lint.py --selftest            the planted twins + the re-found bugs (exit 1 if one is missed)

Checks (a route is the .txt; its flags are its '# harness: flags ...' header lines, as run_list.ps1 merges them):
  (a) every --flag names a cvar the code defines: REXCVAR_DEFINE_* in src\ and the SDK source (thirdparty\rexglue-sdk-src, found from the main checkout when run in a worktree).
      A flag not defined = ERROR, unless the exe being run (or a DLL in its folder) holds the name as a string; WARN when --exe is not under this checkout (a misspelt switch makes the game refuse its command line or silently ignore it).
  (b) dev-only flags (DEV_ONLY below: the sentinel, forced inputs, traces, the case ring ...) on a RELEASE exe (--exe under out\release or release\build) = ERROR for force_inputs / sentinel (ignored there), WARN for the diagnostics (trace, coverage, dump_targets, census, file_trace).
  (c) the FINAL flag line is printed. A route header line '# intent: owner on, scene 2, sentinel 30' is checked against it: every term must hold in the final flags
      (terms: '<name> on|off|<value>' resolved as wwe2k14_rdr_<name> / wwe2k14_<name> / <name> / any wwe2k14 cvar ending _<name>; 'flag --x[=v]'; 'no flag --x'). A mismatch = ERROR.
      A route with no intent line = WARN (--verbose lists them).
      S108 OPT: SAVE / MATCH terms 'blood on|off', 'captions on|off', 'unlocks all|any' are checked against how the route STARTS its match (direct launch / @quick /
      match builder with its o= overrides / menus) and the user-data mirror (--save PATH or name; default userdata_frozen). A start that cannot satisfy a term = ERROR
      with what to change; a term the lint cannot check (menus, an unreadable save) = ERROR 'not checkable', never waved through.
  (e) S109 FF: normal speed has ONE door, the route's intent term `performance <what it measures>` (the run must hold the machine: run_list passes --exclusive; a shared
      run = ERROR) or `watch` (a run a person plays / watches; --parallel = ERROR). ERROR: the removed realtime header line; --vsync=true in the final flags without
      performance / watch / `flag --vsync=true`; and (fast-forwarded, not in replay mode: --wwe2k14_replay_mode / _play / _record) the two step shapes fast-forward moves:
      a shot / clip at a frame count after an entrance / movie sync, and a press at a frame count after a page change (wait_file / wait_text / state menu|seq) with no
      menu_* / cursor / other state sync between. WARN: an in-route `cvar vsync true` without performance. Standalone (no --final) the hold checks are skipped.
  (d) WARN: a route using the `bleed` verb or naming blood in its `# Question` lines while starting through a direct launch (a captured match is blood off: use o=81:1).
  (d) run_list.ps1 hands the final line here before every game start and refuses on an ERROR; its hidden additions are printed ('[run_list] HIDDEN ADDITION: ...').
  Standalone (no --final) the lint adds run_list's one semantic addition itself: --wwe2k14_s101_promotions=false unless the flags name rdr_owner or s101_promotions (run 25 ran the OLD renderer by it)."""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', 'run'))   # S109 FF: route_header (the route's steps, includes expanded)
import stream_cvars   # S105 H: the stream-only cvar list, read from the code
import save_facts     # S108 OPT: the save's options (read-only) and how the match is started
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
FLAG_RX = re.compile(r'(?<![\w-])--([A-Za-z][\w.]*)(?:=(\S*))?')
DEF_RX = re.compile(r'REXCVAR_DEFINE_[A-Z0-9_]+\(\s*(\w+)\s*,\s*([^,]*?)\s*,')
# flags only a +dev build honours or that only make sense there (one regex each, matched against the flag name)
DEV_ERROR = [r'rdr_scene_force_inputs', r'rdr_scene_sentinel']   # a release exe ignores them: the run silently measures something else = ERROR
DEV_WARN = [r'trace', r'coverage', r'^wwe2k14_case(_dir)?$', r'dump_targets', r'rdr_scene_census', r'file_trace']   # diagnostics: WARN
SKIP_DIRS = ('android',)   # phone routes carry adb-side flags the PC game does not define


def sdk_src(root):
    for c in (os.environ.get('WWE_SDK_SRC'), os.path.join(root, 'thirdparty', 'rexglue-sdk-src')):
        if c and os.path.isdir(c):
            return c
    try:   # a worktree: the main checkout holds thirdparty
        common = subprocess.run(['git', '-C', root, 'rev-parse', '--git-common-dir'], capture_output=True, text=True).stdout.strip()
        main = os.path.dirname(os.path.abspath(os.path.join(root, common)))
        c = os.path.join(main, 'thirdparty', 'rexglue-sdk-src')
        if os.path.isdir(c):
            return c
    except OSError:
        pass
    return None


_CV = {}


def cvars(root):
    """({name: default text}, sdk_found); the SDK ones only when its source is found."""
    if root in _CV:
        return _CV[root]
    out, sdk = {}, sdk_src(root)
    for base in [os.path.join(root, 'src')] + ([os.path.join(sdk, 'src'), os.path.join(sdk, 'include')] if sdk else []):
        for d, dirs, files in os.walk(base):
            for f in files:
                if f.endswith(('.h', '.cpp', '.hpp', '.cc', '.inc')):
                    try:
                        t = open(os.path.join(d, f), encoding='utf-8', errors='replace').read()
                    except OSError:
                        continue
                    if 'REXCVAR_DEFINE' in t:
                        for m in DEF_RX.finditer(t):
                            out.setdefault(m.group(1), m.group(2).strip().strip('"'))
    _CV[root] = (out, bool(sdk))
    return _CV[root]


def header_lines(text):
    return text.split('\n')[:40]


def header_flags(text):
    toks = {}
    for ln in header_lines(text):
        m = re.match(r'^#\s*harness:\s*flags\s+(.+)$', ln.strip())
        if m:
            for t in m.group(1).split():
                if t.startswith('--'):
                    toks[t.split('=', 1)[0]] = t   # the last wins, as run_list
    return list(toks.values())


def intent(text):
    for ln in header_lines(text):
        m = re.match(r'^#\s*intent:\s*(.+)$', ln.strip())
        if m:
            return [t.strip() for t in m.group(1).split(',') if t.strip()]
    return None


HARNESS_REALTIME_RX = re.compile(r'^#\s*harness:\s*realtime\b')   # S109 FF review: a plain literal, so a grep for the header finds this reader
SPEED_TERM_RX = re.compile(r'^(performance\b.*|watch)$')   # S109 FF: the ONE door for normal speed: `performance <number>` or `watch`
VSYNC_TERM_RX = re.compile(r'^flag\s+--?vsync=(true|1|on)$')
REPLAY_FLAGS = ('wwe2k14_replay_mode', 'wwe2k14_replay_play', 'wwe2k14_replay_record')   # det_frames: every guest clock follows frames
SYNC_STEPS = {'wait_state', 'wait_text', 'press_until_state', 'press_until_text', 'wait_file', 'press_until_file', 'wait_event', 'press_until_preview'}
MATCH_FILES = ('m3.pac',)   # a wait_file on this = the match loaded (it opens inside the match loading screen, entrances over, ~50 frames before the fade: src\load_turbo.h:15-22, :147): frame-stepped from there.
# INGAMELUA.PAC is NOT a match start: it opens at the in-game sequence start, before the loading screen and both entrances (src\load_turbo.h:14-18)
ENTRANCE_FILES = ('.bik', 'ingamelua', 'titantron')   # a wait_file on these = an entrance / movie follows (guest-clock timed)
CONFIRM_PRESSES = ('a', 'start')   # a plain press that opens a page (the S109 FF2 'opened' shape)
CLOCK_SHOT_STEPS = {'screenshot', 'screenshot_every', 'shot', 'record', 'clip_start'}


def strip_speed_intent(lines):
    """S109 FF2: the one shared helper for route GENERATORS that copy a source route's lines: the source's `# intent:` speed terms (`performance ...`, `watch`) are not the
    copy's (a bench route's `# intent: performance fps` copied into a determinism / coverage / review-stills route made it a timed run). Other intent terms stay; an intent line
    left with no term goes. A generator that makes a timed route says so itself (its own `# intent: performance <what>` line)."""
    out = []
    for ln in lines:
        m = re.match(r'^(#\s*intent:\s*)(.+)$', ln.strip())
        if m:
            keep = [t.strip() for t in m.group(2).split(',') if t.strip() and not SPEED_TERM_RX.match(t.strip())]
            if len(keep) != len([t for t in m.group(2).split(',') if t.strip()]):
                if keep:
                    out.append(m.group(1) + ', '.join(keep))
                continue
        out.append(ln)
    return out


def header_realtime(text):
    """The first line of the removed realtime header kind in the header (S109 FF: that header no longer exists: normal speed has one door, the intent term)."""
    for n, ln in enumerate(header_lines(text), 1):
        if HARNESS_REALTIME_RX.match(ln.strip()):
            return n
    return None


def speed_terms(it):
    """(performance term or None, watch?) from the intent terms."""
    perf = next((t for t in (it or []) if re.match(r'^performance\b', t)), None)
    return perf, any(t == 'watch' for t in (it or []))


def replay_flag(flags):
    return any(flags.get(n) is not None and str(flags[n]).lower() not in ('', '0', 'false', 'off', 'no') for n in REPLAY_FLAGS)


def step_state_name(st):
    a = st.args.split()
    if st.cmd == 'wait_state' and a:
        return a[0]
    if st.cmd == 'press_until_state' and len(a) > 1:
        return a[1]
    return ''


def ff_step_findings(path, root, flags):
    """S109 FF: the two step shapes fast-forward moves (a frame count laid over something the guest clock or a page load times).
    (R2) a shot / clip at a frame count after an entrance / movie sync (wait_file *.bik*, an entrance state) with no state sync between;
    (R3) a `press` at a frame count after a page-change sync (wait_file, wait_text, state `menu` / `seq`) with no `menu_*` / `cursor` /
    other state sync between. Replay mode (det_frames) makes guest time follow frames: both shapes are safe there."""
    import route_header
    try:
        r = route_header.parse_route(path, root)
    except (OSError, FileNotFoundError):
        return []
    ctx = None       # None | 'ent' | 'page' | 'title' | 'cur'
    normal = False   # an in-route `cvar vsync true` window: normal speed until `cvar vsync false` (nothing fast-forwards there)
    where = None     # (file, line) of the sync
    pend = None      # (file, line) of the first wait_frames since that sync
    title_seen = False
    out = {'R2': [], 'R3': []}
    for st in r.steps:
        c = st.cmd
        if c == 'cvar' and st.args.split()[:1] == ['vsync'] and len(st.args.split()) > 1:
            normal = truthy(st.args.split()[1])
            continue
        if normal:
            ctx, pend = None, None
            continue
        if c in SYNC_STEPS:
            name = step_state_name(st)
            a = st.args.lower()
            if name in ('menu_ready', 'menu_count'):
                continue   # menu_ready is 1 on nearly every menu frame (docs/research/s108/MENU_route_values.md section 3.1): it never reports a page change, so it syncs nothing
            if c in ('wait_file', 'press_until_file') and any(m in a for m in MATCH_FILES):
                ctx = None   # the match loaded: it is stepped per frame
            elif (c in ('wait_file', 'press_until_file') and any(m in a for m in ENTRANCE_FILES)) or (name and 'entrance' in name.lower()):
                ctx = 'ent'
            elif c == 'wait_file' and 'savedata.dat' in a and not title_seen:
                # S109 FF2: the title block (`wait_file SaveData.Dat`, a frame count, START as `press START` or `set 0x0010`) is SAFE fast-forwarded, for both verbs: the boot turbo window
                # ends AT SaveData.Dat (src\load_turbo.h:24-25), so the title presses already run at normal speed or in a settled window; 214 routes carrying `# harness: fast-forward ok` run it
                # fast-forwarded, and S108's fast-forwarded runs 35 / 36 passed it. (Confirmed by the main session in the check round.) Only the FIRST SaveData.Dat sync is the title.
                ctx = 'title'
                title_seen = True
            elif c in ('wait_file', 'press_until_file', 'wait_text', 'press_until_text') or name in ('menu', 'seq'):
                ctx = 'page'
            elif name == 'cursor' or name.startswith('menu_'):
                ctx = 'cur'   # a menu page the route is synced on; a plain confirm press from here may open the next page (S109 FF2)
            else:
                ctx = None   # roster_ready / match states: a state sync of its own
            where, pend = (st.file, st.line), None
        elif c == 'wait_frames' and ctx and pend is None:
            try:
                if int(st.args.split()[0]) > 0:
                    pend = (st.file, st.line)
            except (ValueError, IndexError):
                pass
        elif ctx == 'ent' and c in CLOCK_SHOT_STEPS:
            if c == 'screenshot_every' and st.args.split()[:1] == ['0']:
                continue
            if pend is not None:
                out['R2'].append(f'{c} at {st.file}:{st.line} follows wait_frames at {pend[0]}:{pend[1]} (sync at {where[0]}:{where[1]})')
        elif c in ('press', 'set'):
            a = st.args.split()
            if c == 'set':   # a `set` with an empty mask (release) presses nothing
                try:
                    if not a or int(a[0], 0) == 0:
                        continue
                except ValueError:
                    continue
            if ctx == 'page' and pend is not None:
                out['R3'].append(f'{c} {st.args} at {st.file}:{st.line} (wait_frames at {pend[0]}:{pend[1]} after the sync at {where[0]}:{where[1]})')
            elif ctx == 'cur' and c == 'press' and a and a[0].lower() in CONFIRM_PRESSES:
                ctx, where, pend = 'page', (st.file, st.line), None   # S109 FF2: this press opens a page; the page load is not a sync
    res = []
    if out['R2']:
        res.append(f'fast-forward moves a shot / clip laid at a frame count after an entrance / movie sync ({len(out["R2"])}x, first: {out["R2"][0]}): '
                   'the movie and the entrance follow the guest clock, so the frame count lands on another moment at another speed. Sync on a state, '
                   'or run in replay mode (--wwe2k14_replay_mode=true)')
    if out['R3']:
        res.append(f'fast-forward moves a press laid at a frame count after a page change ({len(out["R3"])}x, first: {out["R3"][0]}): '
                   'a press the page is not ready for is dropped. Sync each press on `wait_state menu_page <word>` / `menu_row` / `press_until_state ... menu_row` (menu_ready is a settle, not a page signal), '
                   'or run in replay mode (--wwe2k14_replay_mode=true)')
    return res


def parse_flags(line):
    d = {}
    for m in FLAG_RX.finditer(line):
        d[m.group(1)] = m.group(2) if m.group(2) is not None else 'true'
    return d


def hidden_additions(flags):
    add = []
    if not re.search(r'--wwe2k14_(rdr_owner|s101_promotions)=', flags):
        add.append('--wwe2k14_s101_promotions=false')
    return add


def truthy(v):
    return str(v).lower() in ('1', 'true', 'on', 'yes')


def holds(val, got):
    v = val.lower()
    return truthy(got) if v == 'on' else (not truthy(got)) if v == 'off' else got == val


def term_check(term, flags, defs):
    """None if the term holds, else why not."""
    m = re.match(r'^(no )?flag\s+--?([\w.]+)(?:=(\S+))?$', term)
    if m:
        neg, name, val = m.groups()
        have = name in flags
        if neg:
            return None if not have else f"'{term}': --{name} is set"
        if not have:
            return f"'{term}': --{name} is not in the final flags"
        return None if val is None or flags[name] == val else f"'{term}': --{name}={flags[name]}"
    m = re.match(r'^([\w.]+)\s+(\S+)$', term)
    if not m:
        return f"'{term}': not understood (use '<name> on|off|<value>')"
    name, val = m.groups()
    cands = [f'wwe2k14_rdr_{name}', f'wwe2k14_{name}', name]
    cands += sorted(k for k in list(flags) + list(defs) if k.startswith('wwe2k14') and k.endswith('_' + name))
    seen = []
    for c in cands:
        if c in seen:
            continue
        seen.append(c)
        if c in flags:
            return None if holds(val, flags[c]) else f"'{term}': --{c}={flags[c]} in the final flags"
    for c in seen:   # not on the final line: defaults are decided at run time (S101 promotions set ~30 switches), never read from source text
        if c in defs:
            return f"'{term}': --{c} is not on the final flag line: set --{c}=... explicitly in the header (defaults are decided at run time)"
    return f"'{term}': no flag or cvar named like '{name}'"


def save_term(term, how, sv):
    """S108 OPT: a SAVE / MATCH intent term -> None if it holds, else why not (with what to change).
    how = save_facts.launch_how(...), sv = save facts or {'error': why}."""
    kind, val = re.match(r'^(blood|captions|unlocks)\s+(\S+)$', term).groups()
    if kind == 'unlocks':
        if val not in ('all', 'any'):
            return f"'{term}': not understood (use 'unlocks all' or 'unlocks any')"
    elif val not in ('on', 'off'):
        return f"'{term}': not understood (use '{kind} on' or '{kind} off')"
    want = val == 'on'
    if kind == 'blood':
        k = how['kind']
        if k == 'direct':
            if want:
                return f"'{term}': a direct launch ({how['name']}) restores the captured match, which is blood off whatever the save says; use the match builder with o=81:1"
            return None
        if k == 'builder':
            if 81 in how['o']:
                return None if bool(how['o'][81]) == want else f"'{term}': the match builder has o=81:{how['o'][81]}; change it to o=81:{int(want)}"
            return f"'{term}': the match builder without o=81 takes blood from the save (not proven); add o=81:{int(want)}"
        if k == 'quick':
            if 'error' in sv:
                return f"'{term}': not checkable: @quick takes blood from the save and the save cannot be read ({sv['error']})"
            if bool(sv['blood']) == want:
                return None
            return f"'{term}': @quick takes blood from the save, which is blood {save_facts.onoff(sv['blood'])} (S+81); use the match builder with o=81:{int(want)}"
        if k == 'both':
            return f"'{term}': not checkable: the route starts a direct launch AND a match builder line"
        return f"'{term}': not checkable: the route starts no match by a launch or the match builder (menus: blood is whatever the menu holds); use the match builder with o=81:{int(want)}"
    if 'error' in sv:
        return f"'{term}': not checkable: the save cannot be read ({sv['error']})"
    if kind == 'captions':
        if bool(sv['caption']) == want:
            return None
        return f"'{term}': the save's 30 Years caption word is {save_facts.onoff(sv['caption'])}; run on a test save made with tools\\save\\make_test_save.py --caption {int(want)} (-Data NAME)"
    n, tot = sv['unlock'], sv['unlock_total']
    if (val == 'all' and n == tot) or (val == 'any' and n > 0):
        return None
    return f"'{term}': the save has {n}/{tot} unlock flags set; run on a save made with tools\\save\\unlock_all.py (-Data NAME)"


def bleed_warning(text, how):
    """S108 OPT: the `bleed` verb, or blood in the route's `# Question` lines, on a direct launch -> one line (else None)."""
    if how['kind'] not in ('direct', 'both'):
        return None
    verb = any(re.match(r'^\s*bleed\b', ln) for ln in text.split('\n'))
    ask = inq = False
    for ln in text.split('\n')[:40]:
        t = ln.strip()
        if re.match(r'^#\s*Question', t):
            inq = True
        elif not t.startswith('#') or re.match(r'^#\s*(Read|Reference|Control|harness|intent|Session)\b', t):
            inq = False
        if inq and re.search(r'blood', t, re.I):
            ask = True
    if not (verb or ask):
        return None
    return ('uses the `bleed` verb' if verb else 'names blood in its question') + f" but starts through a direct launch ({how['name']}): a captured match is blood off; start with the match builder and o=81:1"


_EXE = {}


def exe_knows(exe, name):
    """True when the exe or a DLL in its folder holds the flag name as an ASCII string (a branch-only cvar of a worktree / SDK-worktree exe)."""
    import mmap
    if not exe or not os.path.isfile(exe):
        return False
    folder = os.path.dirname(os.path.abspath(exe))
    if folder not in _EXE:
        paths = [exe] + [os.path.join(folder, f) for f in os.listdir(folder) if f.lower().endswith('.dll')]
        maps = []
        for q in paths:
            try:
                fh = open(q, 'rb')
                if os.path.getsize(q):
                    maps.append(mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ))
            except OSError:
                pass
        _EXE[folder] = maps
    needle = name.encode()
    return any(m.find(needle) >= 0 for m in _EXE[folder])


def under_root(exe, root):
    try:
        return os.path.commonpath([os.path.abspath(exe), os.path.abspath(root)]) == os.path.abspath(root)
    except ValueError:
        return False


def is_release(exe):
    e = (exe or '').lower().replace('/', '\\')
    return '\\out\\release\\' in e or 'release\\build' in e or e.startswith('release\\')


def input_names(root):
    """Input names the mask switches accept (inputs_core.h's table: {kInXxx, "name", ...})."""
    p = os.path.join(root, 'src', 'scene_live', 'inputs_core.h')
    try:
        return set(re.findall(r'\{\s*kIn\w+\s*,\s*"(\w+)"', open(p, encoding='utf-8', errors='replace').read()))
    except OSError:
        return set()


def lint_route(path, root, final=None, exe=None, extra='', save=None, exclusive=None, parallel=None):
    """(errors, warns, final flag line, hidden additions). save: a user-data mirror (path / name), save facts, or None = userdata_frozen; read only when a save term needs it.
    exclusive / parallel (S109 FF): how run_list holds the machine (None = standalone lint, not known: the `performance` / `watch` hold checks are skipped)."""
    text = open(path, encoding='utf-8', errors='replace').read()
    errs, warns = [], []
    defs, sdk_found = cvars(root)
    hdr = header_flags(text)
    hidden = []
    if final is None:
        base = ' '.join(hdr + ([extra] if extra else []))
        hidden = hidden_additions(base)
        final = (base + ' ' + ' '.join(hidden)).strip()
    flags = parse_flags(final)
    for name in list(parse_flags(' '.join(hdr))) + (list(parse_flags(extra)) if extra else []):
        if name in defs or not (name.startswith('wwe2k14_') or sdk_found):
            continue
        if exe_knows(exe, name):
            continue   # a branch-only cvar the exe being run has
        msg = f'--{name}: no REXCVAR_DEFINE in src\\ or the SDK source (misspelt, or removed)'
        if exe and not under_root(exe, root):
            warns.append(msg + f'; the exe {exe} is not under this checkout and does not hold the name either (a branch-only cvar of a worktree exe would)')
        else:
            errs.append(msg)
    # S105 H: a route run on the STREAM-FREE exe (-Exe ...release-off...) must not set a stream-only cvar: that exe refuses it ("cvar ... was not set")
    # and the route aborts at the line (sweep runs 045 / 047 / 048 / 052 printed PASS and never ran their mode). The list comes from the code (stream_cvars.py).
    if exe and (stream_cvars.is_stream_free_exe(exe) or is_release(exe)):   # the OFF build folder, or a shipped player exe (always stream-free)
        try:
            only = stream_cvars.stream_only_cvars(root)
        except Exception as ex:   # a broken q module must never stop a run: say so and skip this check
            only = {}
            warns.append(f'stream-only cvar check skipped ({type(ex).__name__}: {ex})')
        seen_sf = set()
        for n_ in stream_cvars.route_cvar_names_deep(path):   # `cvar` lines, included routes too: the exe REFUSES them and the route stops
            if n_ in only and n_ not in seen_sf:
                seen_sf.add(n_)
                errs.append(f'stream-only cvar "{n_}" ({only[n_]}) set by a `cvar` line on the stream-free exe ({exe}): the exe refuses it and the route stops at that line')
        for n_ in flags:   # a command-line flag is NOT refused (the SDK stashes unknown options): the switch is silently dropped
            if n_ in only and n_ not in seen_sf:
                seen_sf.add(n_)
                warns.append(f'--{n_} is a stream-only cvar ({only[n_]}): the stream-free exe ignores it silently, the run does not have it')
    if is_release(exe):
        for name in flags:
            if any(re.search(p, name) for p in DEV_ERROR):
                errs.append(f'--{name}: dev-only flag on a release exe ({exe}): it is ignored there')
            elif any(re.search(p, name) for p in DEV_WARN):
                warns.append(f'--{name}: diagnostic flag on a release exe ({exe})')
    # S103: the input-mask switches take bit numbers or names from src\scene_live\inputs_core.h; an unknown token is silently
    # dropped by the game (run 33 used 'crowd' and unrequired nothing).
    names = input_names(root)
    for sw in ('wwe2k14_rdr_scene_unrequire', 'wwe2k14_rdr_scene_hold'):
        if sw in flags and names:
            for tok in [t.strip() for t in str(flags[sw]).strip('"').split(',') if t.strip()]:
                if not tok.isdigit() and tok not in names:
                    errs.append(f'--{sw}: unknown token "{tok}" (the game ignores it; names: {", ".join(sorted(names))}, or a bit number)')
    it = intent(text)
    # S109 FF: normal speed has ONE door, the intent term `performance <number>` / `watch`.
    hn = header_realtime(text)
    if hn:
        errs.append(f'line {hn}: the realtime header no longer exists (it is a comment that changed speed): a timed route says `# intent: performance <what it measures>`, '
                    'a run a person plays / watches says `# intent: watch`, any other route is fast-forwarded')
    perf, watch = speed_terms(it)
    if perf and exclusive is False:
        errs.append(f"intent '{perf}': a performance run must hold the machine (run with -Exclusive or -Alone): a shared run is not a measurement")
    if watch and parallel:
        errs.append("intent 'watch': a run a person watches is not run with -Parallel")
    vs = flags.get('vsync')
    if vs is not None and truthy(vs) and not (perf or watch or any(VSYNC_TERM_RX.match(t) for t in (it or []))):
        errs.append('--vsync=true in the final flags without a speed term: add `performance <number>` or `watch` to the route\'s `# intent:` line (or `flag --vsync=true` for an A/B arm)')
    if not (perf or watch) and not (vs is not None and truthy(vs)) and not replay_flag(flags):
        errs.extend(ff_step_findings(path, root, flags))   # fast-forwarded and not in replay mode
    if not perf and re.search(r'^\s*cvar\s+vsync\s+(true|1|on)\b', text, re.M | re.I):
        warns.append('a `cvar vsync true` step switches to normal speed inside the route without a `performance` term (references taken at normal speed)')
    if it is None:
        warns.append('no "# intent:" header line (what the run must have on, e.g. "# intent: owner on, scene 2, sentinel 30")')
    else:
        how = save_facts.launch_how(text, flags)
        sv = save
        for t in it:
            if SPEED_TERM_RX.match(t):
                continue   # speed terms (checked above), like the save terms below they are not flag terms
            if re.match(r'^(blood|captions|unlocks)\s+\S+$', t):
                if not isinstance(sv, dict):
                    sv = save_facts.load(sv or 'userdata_frozen')   # read only when a term needs it
                why = save_term(t, how, sv)
            else:
                why = term_check(t, flags, defs)
            if why:
                errs.append('intent mismatch ' + why + (f'; run_list adds {" ".join(hidden)}' if hidden else ''))
    if not (it and any(re.match(r'^blood\s', t) for t in it)):   # a route that states its blood term has decided
        bw = bleed_warning(text, save_facts.launch_how(text, flags))
        if bw:
            warns.append(bw)
    return errs, warns, final, hidden


def routes(root):
    base = os.path.join(root, 'tools', 'input_scripts')
    for d, dirs, files in os.walk(base):
        dirs[:] = [x for x in dirs if x not in SKIP_DIRS]
        for f in sorted(files):
            if f.endswith('.txt'):
                yield os.path.join(d, f)


def lint_all(root, verbose=False, exe=None, show_plain=False):
    """exe: a stream-free exe path (--stream-free) also checks every route's cvar lines against the stream-only list."""
    nerr = nwarn = n = n_plain = 0
    for p in routes(root):
        text = open(p, encoding='utf-8', errors='replace').read()
        plain = not header_flags(text) and intent(text) is None and not (exe and stream_cvars.route_cvar_names_deep(p))
        if plain:   # a plain route (no flags, no intent): nothing of ours to check here. S109 FF: run_list's per-run lint still scans its steps; --plain lists the ones it would refuse
            if show_plain and (header_realtime(text) or ff_step_findings(p, root, {})):
                n_plain += 1
                print(f'PLAIN {os.path.relpath(p, root)}: the per-run lint refuses it fast-forwarded (' + '; '.join(x[:60] for x in ff_step_findings(p, root, {})) + ')')
            continue
        n += 1
        errs, warns, final, hidden = lint_route(p, root, exe=exe)
        rel = os.path.relpath(p, root)
        for e in errs:
            print(f'ERROR {rel}: {e}')
            nerr += 1
        if verbose:
            for w in warns:
                print(f'WARN  {rel}: {w}')
        nwarn += len(warns)
    note = '' if cvars(root)[1] else '; SDK source NOT found: only wwe2k14_ flags checked'
    print(f'route_lint: {n} routes with header flags, {nerr} ERROR, {nwarn} WARN (no intent line; --verbose lists them){note}')
    if show_plain:
        print(f'route_lint --plain: {n_plain} plain routes (no flags, no intent) carry a step shape the per-run lint refuses fast-forwarded')
    return nerr


def selftest(root):
    import tempfile
    ok = True
    d = tempfile.mkdtemp(prefix='route_lint_')

    def w(name, body):
        p = os.path.join(d, name)
        open(p, 'w').write(body)
        return p

    def expect(label, path, want_err, **kw):
        nonlocal ok
        errs, warns, final, hidden = lint_route(path, root, **kw)
        good = bool(errs) == want_err
        print(f"  {'ok  ' if good else 'FAIL'} {label}: {len(errs)} error(s) {errs[:1]}")
        ok &= good
        return final
    base = '--wwe2k14_launch=lesnar_bryan --wwe2k14_rdr_scene=2 --wwe2k14_rdr_scene_sentinel=30 --wwe2k14_rdr_owner=true --wwe2k14_s101_promotions=true'
    expect('clean route with intent', w('clean.txt', f'# harness: flags {base}\n# intent: owner on, scene 2, sentinel 30, promotions on\n'), False)
    # re-find run 25: the route forgot the owner switch, run_list added promotions=false, the intent says owner on
    base25 = base.replace('--wwe2k14_rdr_owner=true ', '').replace('--wwe2k14_s101_promotions=true', '')
    final = expect('run 25 (owner forgotten, promotions=false added silently)', w('r25.txt', f'# harness: flags {base25}\n# intent: owner on, scene 2\n'), True)
    ok &= 'promotions=false' in final
    # re-find run 33: --wwe2k14_rdr_scene_unrequire=crowd ('crowd' is not an input name: the game dropped it and unrequired nothing)
    expect('run 33 (unrequire=crowd, unknown token)', w('r33.txt', f'# harness: flags {base} --wwe2k14_rdr_scene_unrequire=crowd\n# intent: owner on\n'), True)
    expect('unrequire=crowd_snapshot,4 (known)', w('r33ok.txt', f'# harness: flags {base} --wwe2k14_rdr_scene_unrequire=crowd_snapshot,4\n# intent: owner on\n'), False)
    expect('misspelt flag (rdr_scene_sentinal)', w('typo.txt', '# harness: flags --wwe2k14_rdr_scene_sentinal=30\n'), True)
    expect('sentinel on a release exe', w('dev.txt', f'# harness: flags {base}\n# intent: owner on\n'), True, exe=r'C:\x\out\release\build\wwe2k14.exe')
    expect('same route on the +dev exe', w('dev2.txt', f'# harness: flags {base}\n# intent: owner on\n'), False, exe=r'C:\x\out\build\win-amd64-release\wwe2k14.exe')
    expect('final line given (run_list) disagrees', w('fin.txt', f'# harness: flags {base}\n# intent: owner on, promotions off\n'), True, final=base)
    e, wn, f, h = lint_route(w('nointent.txt', f'# harness: flags {base}\n'), root)
    good = (not e) and bool(wn)
    print(f"  {'ok  ' if good else 'FAIL'} no intent line: warn only {wn[:1]}")
    ok &= good
    e, wn, f, h = lint_route(w('trace2.txt', '# harness: flags --wwe2k14_trace=true --wwe2k14_rdr_owner=true\n# intent: owner on\n'), root, exe=r'C:\x\out\release\build\wwe2k14.exe')
    good = (not e) and any('trace' in x for x in wn)
    print(f"  {'ok  ' if good else 'FAIL'} trace on a release exe: WARN only {wn[:1]}")
    ok &= good
    # a branch-only cvar: unknown in the source, but a fake exe holds the string -> accepted; an exe outside the checkout without it -> WARN only
    fx = os.path.join(d, 'fake.exe')
    open(fx, 'wb').write(b'MZ\0\0--wwe2k14_branch_only_flag_x\0\0')
    r = w('branch.txt', '# harness: flags --wwe2k14_branch_only_flag_x=1\n')
    e1 = lint_route(r, root, exe=fx)
    good = not e1[0] and not any('branch_only' in x for x in e1[1])
    print(f"  {'ok  ' if good else 'FAIL'} branch-only cvar found in the exe: {e1[0][:1]}")
    ok &= good
    r2 = w('branch2.txt', '# harness: flags --wwe2k14_branch_only_flag_y=1\n')
    e2 = lint_route(r2, root, exe=fx)
    good = not e2[0] and any('branch_only_flag_y' in x for x in e2[1])
    print(f"  {'ok  ' if good else 'FAIL'} unknown flag with an exe outside the checkout: WARN only {e2[1][:1]}")
    ok &= good
    e3 = lint_route(r2, root)
    good = bool(e3[0])
    print(f"  {'ok  ' if good else 'FAIL'} the same unknown flag with no exe: ERROR")
    ok &= good
    e4 = lint_route(w('nodefault.txt', '# harness: flags --wwe2k14_launch=x\n# intent: owner on\n'), root)
    good = bool(e4[0]) and 'explicitly' in e4[0][0]
    print(f"  {'ok  ' if good else 'FAIL'} intent never reads source defaults: {e4[0][:1]}")
    ok &= good
    # S105 H: a stream-only cvar on the stream-free exe (re-finds sweep runs 045 / 047 / 048 / 052: `cvar wwe2k14_rdr_live_picture`)
    off_exe = r'C:\x\out\build\win-amd64-release-off\wwe2k14.exe'
    on_exe = r'C:\x\out\build\win-amd64-release\wwe2k14.exe'
    live = w('live.txt', '# harness: flags --wwe2k14_launch=x --wwe2k14_rdr_owner=true --wwe2k14_s101_promotions=true\n# intent: owner on\ncvar wwe2k14_rdr_live_picture sheet\nquit\n')
    expect('stream-only cvar line on the stream-free exe (planted)', live, True, exe=off_exe)
    expect('the same route on the stream (ON) exe', live, False, exe=on_exe)
    hdr = '# harness: flags --wwe2k14_launch=x --wwe2k14_rdr_owner=true --wwe2k14_s101_promotions=true'
    # review F4: a command-line flag is dropped silently by the exe (not refused): WARN only
    e5 = lint_route(w('liveflag.txt', hdr + ' --wwe2k14_rdr_live_picture=sheet\n# intent: owner on\n'), root, exe=off_exe)
    good = (not e5[0]) and any('live_picture' in x for x in e5[1])
    print(f"  {'ok  ' if good else 'FAIL'} stream-only --flag on the stream-free exe: WARN only {e5[1][:1]}")
    ok &= good
    expect('stream-only cvar line on a shipped player exe (planted, F5)', live, True, exe=r'C:\x\out\release\build\wwe2k14.exe')
    expect('stream-only cvar line on an OFF folder other than release-off (planted, F5 backslash regex)', live, True, exe=r'C:\x\out\build\win-amd64-debug-off\wwe2k14.exe')
    os.makedirs(os.path.join(d, 'nav'), exist_ok=True)
    w(os.path.join('nav', 'sub_live.txt'), 'cvar wwe2k14_rdr_live_picture sheet\n')
    expect('stream-only cvar in an included route (planted, F5)', w('inc.txt', hdr + '\n# intent: owner on\ninclude nav/sub_live\nquit\n'), True, exe=off_exe)
    # review: a broken stream_cvars scan (qx5 import) must never block a run
    real_scan = stream_cvars.stream_only_cvars

    def broken(_root):
        raise ImportError('planted qx5 failure')
    stream_cvars.stream_only_cvars = broken
    e6 = lint_route(live, root, exe=off_exe)
    stream_cvars.stream_only_cvars = real_scan
    good = (not e6[0]) and any('skipped' in x for x in e6[1])
    print(f"  {'ok  ' if good else 'FAIL'} a broken stream_cvars scan never blocks a run: WARN {e6[1][:1]}")
    ok &= good
    expect('a cvar both builds define, on the stream-free exe', w('both.txt', '# harness: flags --wwe2k14_launch=x --wwe2k14_rdr_owner=true --wwe2k14_s101_promotions=true\n# intent: owner on\ncvar wwe2k14_rdr_owner true\nquit\n'), False, exe=off_exe)
    expect('bad intent term', w('badint.txt', f'# harness: flags {base}\n# intent: glorp\n'), True)
    # S108 OPT: the SAVE / MATCH terms. The save is a facts dict built here (blood off, captions off, 0 of 193: the frozen save's values; and the flipped one).
    hb = '# harness: flags --wwe2k14_rdr_owner=true --wwe2k14_s101_promotions=true'
    sv0 = {'blood': 0, 'caption': 0, 'unlock': 0, 'unlock_total': 193, 'dlc': 0, 'hash': '00000000'}
    sv1 = {'blood': 1, 'caption': 1, 'unlock': 193, 'unlock_total': 193, 'dlc': 0, 'hash': '11111111'}
    bld = 'cvar wwe2k14_match type=0;p=139:-1,343:-1;arena=5;o=81:1\ncvar wwe2k14_match_go 108\n'

    def term(label, intent_line, body, want, save=sv0, msg=None, flags=hb):
        """want False = no ERROR; True = ERROR whose first message holds `msg`."""
        nonlocal ok
        errs, warns, _f, _h = lint_route(w('t_%d.txt' % len(os.listdir(d)), f'{flags}\n# intent: {intent_line}\n{body}'), root, save=save)
        good = (bool(errs) == want) and (msg is None or (bool(errs) and msg in errs[0]))
        print(f"  {'ok  ' if good else 'FAIL'} {label}: {len(errs)} error(s) {errs[:1]}")
        ok &= good
        return warns
    launch = '--wwe2k14_launch=lesnar_bryan'
    term('blood on + builder + o=81:1 passes', 'owner on, blood on', bld, False)
    term('blood on + direct launch refused (exact message)', 'owner on, blood on', 'wait 1\n', True, msg="a direct launch (lesnar_bryan) restores the captured match, which is blood off whatever the save says; use the match builder with o=81:1", flags=hb + ' ' + launch)
    term('blood off + direct launch passes (captured match)', 'owner on, blood off', 'wait 1\n', False, flags=hb + ' ' + launch)
    term('blood on + builder WITHOUT o=81 refused', 'owner on, blood on', 'cvar wwe2k14_match type=0;p=139:-1\ncvar wwe2k14_match_go 1\n', True, msg='the match builder without o=81 takes blood from the save (not proven); add o=81:1')
    term('blood on + builder o=81:0 refused', 'owner on, blood on', bld.replace('o=81:1', 'o=81:0'), True, msg='the match builder has o=81:0; change it to o=81:1')
    term('blood off + builder o=81:0 passes', 'owner on, blood off', bld.replace('o=81:1', 'o=81:0'), False)
    term('blood on + @quick on a blood-off save refused', 'owner on, blood on', 'wait 1\n', True, msg='@quick takes blood from the save, which is blood off (S+81)', flags=hb + ' --wwe2k14_launch=@quick')
    term('blood on + @quick on a blood-on save passes', 'owner on, blood on', 'wait 1\n', False, save=sv1, flags=hb + ' --wwe2k14_launch=@quick')
    term('blood on through the menus: not checkable', 'owner on, blood on', 'wait 1\n', True, msg='not checkable')
    term('blood term with an unreadable save on @quick: not checkable', 'owner on, blood off', 'wait 1\n', True, save={'error': 'planted'}, msg='not checkable: @quick takes blood from the save and the save cannot be read (planted)', flags=hb + ' --wwe2k14_launch=@quick')
    term('blood maybe: not understood', 'owner on, blood maybe', bld, True, msg="not understood (use 'blood on' or 'blood off')")
    term('captions on against a captions-off save refused (exact)', 'owner on, captions on', 'wait 1\n', True, msg="the save's 30 Years caption word is off; run on a test save made with tools\\save\\make_test_save.py --caption 1 (-Data NAME)")
    term('captions on against a captions-on save passes', 'owner on, captions on', 'wait 1\n', False, save=sv1)
    term('captions off against a captions-off save passes', 'owner on, captions off', 'wait 1\n', False)
    term('captions with an unreadable save: not checkable', 'owner on, captions on', 'wait 1\n', True, save={'error': 'planted'}, msg='not checkable: the save cannot be read (planted)')
    term('unlocks all on 0/193 refused (exact)', 'owner on, unlocks all', 'wait 1\n', True, msg="the save has 0/193 unlock flags set; run on a save made with tools\\save\\unlock_all.py (-Data NAME)")
    term('unlocks any on 0/193 refused', 'owner on, unlocks any', 'wait 1\n', True, msg='0/193')
    term('unlocks all on 193/193 passes', 'owner on, unlocks all', 'wait 1\n', False, save=sv1)
    term('unlocks any on 3/193 passes', 'owner on, unlocks any', 'wait 1\n', False, save=dict(sv0, unlock=3))
    term('unlocks some: not understood', 'owner on, unlocks some', 'wait 1\n', True, msg="not understood (use 'unlocks all' or 'unlocks any')")
    # the warning (not an error): `bleed` verb / blood in the question on a direct launch
    ws = term('bleed verb on a direct launch: WARN only', 'owner on', 'bleed 0 1\n', False, flags=hb + ' ' + launch)
    good = any('o=81:1' in x and 'bleed' in x for x in ws)
    print(f"  {'ok  ' if good else 'FAIL'} the bleed warning points at o=81:1: {ws[:1]}")
    ok &= good
    ws = term('blood in the question on a direct launch: WARN only', 'owner on', '# Question: does the blood regrow?\n# Read with: x\nwait 1\n', False, flags=hb + ' ' + launch)
    good = any('names blood in its question' in x for x in ws)
    print(f"  {'ok  ' if good else 'FAIL'} question warning: {ws[:1]}")
    ok &= good
    ws = term('bleed verb through the builder: no warning', 'owner on', bld + 'bleed 0 1\n', False)
    good = not any('bleed' in x for x in ws)
    print(f"  {'ok  ' if good else 'FAIL'} no bleed warning on the builder: {ws[:1]}")
    ok &= good
    ws = term('bleed verb with a stated blood term: no warning', 'owner on, blood off', 'bleed 0 1\n', False, flags=hb + ' ' + launch)
    good = not any('bleed' in x for x in ws)
    print(f"  {'ok  ' if good else 'FAIL'} a stated blood term has decided: {ws[:1]}")
    ok &= good
    # the routes of this session carry the terms and pass against the frozen values
    for nm, want in (('s108_bld6_bloodon', False), ('s108_bld7_bloodon', False), ('s108_bld7_bloodoff', False)):
        rp = os.path.join(root, 'tools', 'input_scripts', 'scenes', nm + '.txt')
        if os.path.isfile(rp):
            e, wn, _f, _h = lint_route(rp, root, save=sv0)
            it_ = intent(open(rp, encoding='utf-8', errors='replace').read()) or []
            good = (not e) and any(t.startswith('blood ') for t in it_)
            print(f"  {'ok  ' if good else 'FAIL'} {nm}: blood term present, no error {e[:1]}")
            ok &= good
    # S109 FF: normal speed has one door (the intent term); the header refuses; two step shapes fast-forward moves
    hs = '# harness: flags --wwe2k14_rdr_owner=true --wwe2k14_s101_promotions=true'

    def speed(label, intent_line, body, want, msg=None, header='', flags=hs, **kw):
        nonlocal ok
        errs, warns, _f, _h = lint_route(w('sp_%d.txt' % len(os.listdir(d)), f'{header}{flags}\n# intent: {intent_line}\n{body}'), root, **kw)
        good = (bool(errs) == want) and (msg is None or any(msg in e for e in errs))
        print(f"  {'ok  ' if good else 'FAIL'} {label}: {len(errs)} error(s) {errs[:1]}")
        ok &= good
        return warns
    speed('the removed realtime header is refused', 'owner on', 'wait_frames 1\n', True, msg='no longer exists', header='# harness: realtime (reason)\n')
    speed('performance on a shared run is refused', 'owner on, performance fps', 'wait_frames 1\n', True, msg='must hold the machine', exclusive=False)
    speed('performance on an exclusive run passes', 'owner on, performance fps', 'wait_frames 1\n', False, exclusive=True)
    speed('performance standalone (hold not known) passes', 'owner on, performance fps', 'wait_frames 1\n', False)
    speed('watch with -Parallel is refused', 'owner on, watch', 'wait_frames 1\n', True, msg='-Parallel', parallel=True)
    speed('watch on a single run passes', 'owner on, watch', 'wait_frames 1\n', False, parallel=False)
    speed('--vsync=true without a speed term is refused', 'owner on', 'wait_frames 1\n', True, msg='--vsync=true', flags=hs + ' --vsync=true')
    speed('--vsync=true with `flag --vsync=true` passes', 'owner on, flag --vsync=true', 'wait_frames 1\n', False, flags=hs + ' --vsync=true')
    speed('--vsync=true with performance passes', 'owner on, performance fps', 'wait_frames 1\n', False, flags=hs + ' --vsync=true')
    ent = 'wait_file INGAMELUA.PAC 1800\nwait_file 343.bik2 1800\nwait_frames 120\nscreenshot sc_a\n'
    speed('movie sync + wait_frames + screenshot, fast-forwarded, refused', 'owner on', ent, True, msg='entrance / movie sync')
    speed('the same in replay mode passes', 'owner on', ent, False, flags=hs + ' --wwe2k14_replay_mode=true')
    speed('the same at normal speed (performance) passes', 'owner on, performance frame ms', ent, False)
    speed('movie sync + a state sync + wait_frames + screenshot is still refused (the frame count follows the movie)', 'owner on', 'wait_file 343.bik2 1800\nwait_state entrance_step_0 >=15 3000\nwait_frames 30\nscreenshot sc_a\n', True)
    speed('movie sync, then a screenshot straight after a state sync passes', 'owner on', 'wait_file 343.bik2 1800\nwait_state match_clock >0 3000\nscreenshot sc_a\n', False)
    pg = 'wait_file MNUIHD.PAC 1800\nwait_frames 150\npress DOWN\n'
    speed('press at a frame count after a page-load sync, fast-forwarded, refused', 'owner on', pg, True, msg='after a page change')
    speed('the same after wait_state menu_page <word> passes', 'owner on', 'wait_file MNUIHD.PAC 1800\nwait_frames 150\nwait_state menu_page 0x82038500 300 20\npress DOWN\n', False)
    speed('the same after press_until_state ... menu_row passes', 'owner on', 'wait_file MNUIHD.PAC 1800\nwait_frames 150\npress_until_state DOWN menu_row 3 10 30\npress A\n', False)
    speed('menu_ready is only a settle (1 on nearly every frame): the press is still refused', 'owner on', 'wait_file MNUIHD.PAC 1800\nwait_frames 150\nwait_state menu_ready 1 300 20\npress DOWN\n', True, msg='after a page change')
    speed('a press after a match load (wait_file M3.PAC) + frames is a match press: passes', 'owner on', 'wait_file M3.PAC 1800\nwait_frames 600\npress START\n', False)
    # S109 FF2: the title block is safe for press AND set; set is refused like press after any other page change; INGAMELUA / titantron are entrance syncs; a confirm press opens a page
    title = 'wait_file SaveData.Dat 3600\nwait_frames 160\n'
    speed('FF2 title block, `press START` at a frame count: passes', 'owner on', title + 'press START\nwait_frames 70\npress A\n', False)
    speed('FF2 title block, `set 0x0010` at a frame count: passes', 'owner on', title + 'set 0x0010 0 0 0 0 0 0\nwait_frames 10\nset 0x0000 0 0 0 0 0 0\nwait_frames 66\nset 0x1000 0 0 0 0 0 0\n', False)
    speed('FF2 `set` at a frame count after another page change is refused like press', 'owner on', title + 'set 0x0010 0 0 0 0 0 0\nwait_file MNUIHD.PAC 3600\nwait_frames 111\nset 0x1000 0 0 0 0 0 0\n', True, msg='after a page change')
    speed('FF2 `set 0x0000` (a release) after a page change presses nothing: passes', 'owner on', 'wait_file MNUIHD.PAC 3600\nwait_frames 111\nset 0x0000 0 0 0 0 0 0\n', False)
    speed('FF2 a second SaveData.Dat sync is not the title: its presses are refused', 'owner on', title + 'set 0x0010 0 0 0 0 0 0\nwait_file SaveData.Dat 3600\nwait_frames 60\nset 0x1000 0 0 0 0 0 0\n', True, msg='after a page change')
    speed('FF2 wait_file INGAMELUA.PAC + frames + screenshot is the entrance shape: refused', 'owner on', 'wait_file INGAMELUA.PAC 1800\nwait_frames 600\nscreenshot sc_a\n', True, msg='entrance / movie sync')
    speed('FF2 wait_file titantron + frames + clip is the entrance shape: refused', 'owner on', 'wait_file titantron 1800\nwait_frames 300\nclip_start c\n', True, msg='entrance / movie sync')
    speed('FF2 INGAMELUA.PAC then a match sync (match_clock) then a shot passes', 'owner on', 'wait_file INGAMELUA.PAC 1800\nwait_frames 600\nwait_state match_clock >0 3000\nscreenshot sc_a\n', False)
    speed('FF2 INGAMELUA.PAC then M3.PAC then frames and a shot passes', 'owner on', 'wait_file INGAMELUA.PAC 1800\nwait_file M3.PAC 1800\nwait_frames 300\nscreenshot sc_a\n', False)
    speed('FF2 INGAMELUA.PAC + frames + shot in replay mode passes', 'owner on', 'wait_file INGAMELUA.PAC 1800\nwait_frames 600\nscreenshot sc_a\n', False, flags=hs + ' --wwe2k14_replay_mode=true')
    gf = 'wait_state cursor 3 300 20\npress A\nwait_frames 240\npress DOWN\nwait_frames 30\npress DOWN\n'
    speed('FF2 a confirm press after a cursor sync opens a page, then frame-counted presses: refused (gfx_rows_probe / s54_gfx_res_change)', 'owner on', gf, True, msg='after a page change')
    speed('FF2 cursor moves on an in-memory page (no confirm press) stay safe', 'owner on', 'wait_state cursor 3 300 20\nwait_frames 30\npress DOWN\nwait_frames 30\npress DOWN\n', False)
    speed('FF2 a confirm press then a state sync, then presses: passes', 'owner on', 'wait_state cursor 3 300 20\npress A\nwait_frames 240\nwait_state menu_row 2 300 20\npress DOWN\n', False)
    speed('FF2 the gfx shape at normal speed (performance) passes', 'owner on, performance fps', gf, False)
    speed('a normal-speed window (cvar vsync true) is exempt', 'owner on', 'cvar vsync true\nwait_file MNUIHD.PAC 1800\nwait_frames 150\npress DOWN\n', False)
    speed('the same in replay mode passes', 'owner on', pg, False, flags=hs + ' --wwe2k14_replay_mode=true')
    speed('a press straight after the sync (no frame count) passes', 'owner on', 'wait_file MNUIHD.PAC 1800\npress DOWN\n', False)
    ws = speed('a `cvar vsync true` step without performance: WARN only', 'owner on', 'cvar vsync true\nwait_frames 1\n', False)
    good = any('cvar vsync true' in x for x in ws)
    print(f"  {'ok  ' if good else 'FAIL'} cvar vsync warning: {ws[:1]}")
    ok &= good
    # S109 FF2: the generators' shared helper drops a copied speed term and keeps the others
    got = strip_speed_intent(['# intent: performance fps', '# intent: owner on, performance fps, scene 2', '# intent: watch', '# intent: owner on', 'quit'])
    good = got == ['# intent: owner on, scene 2', '# intent: owner on', 'quit']
    print(f"  {'ok  ' if good else 'FAIL'} strip_speed_intent drops speed terms only: {got}")
    ok &= good
    # every route that carries a performance term holds no speed error (exclusive run)
    n_perf = 0
    for rp in routes(root):
        t_ = open(rp, encoding='utf-8', errors='replace').read()
        if speed_terms(intent(t_))[0]:
            n_perf += 1
            e, _w, _f, _h = lint_route(rp, root, exclusive=True)
            bad = [x for x in e if 'performance' in x or 'vsync' in x or 'realtime' in x]
            if bad:
                print(f'  FAIL {os.path.relpath(rp, root)}: {bad[0][:120]}')
                ok = False
    print(f'  ok   {n_perf} performance routes carry no speed error')
    print('selftest ' + ('PASSED' if ok else 'FAILED'))
    return ok


def main():
    a = sys.argv[1:]
    if '--selftest' in a:
        sys.exit(0 if selftest(ROOT) else 1)

    def opt(n):
        return a[a.index(n) + 1] if n in a else None
    r = opt('--route')
    if r:
        # S109 FF: run_list hands the hold it runs in (--final = a real run): --exclusive when no other game runs alongside, --parallel for -Parallel N > 1
        real = opt('--final') is not None
        errs, warns, final, hidden = lint_route(r, ROOT, final=opt('--final'), exe=opt('--exe'), extra=opt('--extra') or '', save=opt('--save'),
                                                exclusive=('--exclusive' in a) if real else None, parallel=('--parallel' in a) if real else None)
        print('[route_lint] final flags: ' + final)
        for w_ in warns:
            print('[route_lint] WARN ' + w_)
        for e in errs:
            print('[route_lint] ERROR ' + e)
        sys.exit(1 if errs else 0)
    # S105 H: --stream-free lints every route as if run on the stream-free exe (lists the routes that carry a stream-only cvar)
    sys.exit(1 if lint_all(ROOT, '--verbose' in a, show_plain='--plain' in a, exe=(r'out\build\win-amd64-release-off\wwe2k14.exe' if '--stream-free' in a else None)) else 0)


if __name__ == '__main__':
    main()
