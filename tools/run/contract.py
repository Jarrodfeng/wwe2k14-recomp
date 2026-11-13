"""Session 101 (arc A4, tool 8): the run contract = the rules for starting a game run, as data + checks. Import-only (no side effects).

Each rule is a function `rule_N_name(plan) -> [Finding]` with its reason and its source (docs\\LESSONS.md / CLAUDE.md / run_list.ps1) in
a comment. `check_plan(plan)` runs them all. A finding is "refuse" (the door stops) or "warn" (printed, the run goes on).
A route may waive one rule with a header line `# contract: allow RULE-KEY reason` (keys below); the waiver is printed in the plan.

Rule keys:  speed (1)  launch (2)  flags (3)  settings (4)  shots (5)  picture (6)
"""
import difflib
import os
import re
import subprocess
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

from route_header import Route, SHOT_STEPS, STATE_SYNC_STEPS

REPO = Path(__file__).resolve().parents[2]


def _main_checkout(here=REPO):
    # Where out\ and game_data\ live, also from a worktree (same trick as tools\gate\gate.py).
    try:
        common = subprocess.run(['git', '-C', str(here), 'rev-parse', '--path-format=absolute', '--git-common-dir'],
                                capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return Path(common).parent
    except Exception:
        pass
    return here


MAIN = _main_checkout()

# ---------------------------------------------------------------------------------------------------------------- flags
FALSY = {'0', 'false', 'off', 'no', ''}


@dataclass
class Flag:
    name: str                 # '--wwe2k14_launch' (with dashes)
    value: Optional[str]      # None for a bare flag
    source: str               # 'route header line 3' / 'command line' / 'door: black box' / 'kind --speed'
    raw: str = ''

    def text(self):
        return self.name if self.value is None else f'{self.name}={self.value}'

    def truthy(self):
        return self.value is None or self.value.strip().lower() not in FALSY

    @property
    def bare(self):
        return self.name.lstrip('-')


def parse_flags(tokens, source):
    """tokens: a list of strings (or one string, split on spaces). Returns (flags, bad_tokens). A value must use `=` (--a=b)."""
    if isinstance(tokens, str):
        tokens = tokens.split()
    flags, bad = [], []
    for t in tokens:
        if re.match(r'^--[\w.-]+(=.*)?$', t):
            name, eq, val = t.partition('=')
            flags.append(Flag(name, val if eq else None, source, t))
        else:
            bad.append(t)
    return flags, bad


_DEFINE = re.compile(r'REXCVAR_DEFINE_[A-Z0-9_]+\(\s*([A-Za-z0-9_]+)')
_cvar_cache = {}

# Flags the exe takes that are not REXCVAR_DEFINE_* (the SDK's CLI front end). Verified by lint-all: every flag in the 600 routes is
# either a cvar or listed here; add only with a source.
NON_CVAR_FLAGS = {'help', 'version'}


def load_cvars(repo=REPO, main=MAIN):
    """Every cvar name the exe can know: REXCVAR_DEFINE_* in our src\\ and in the SDK source (the wwe2k14-local branch in the main checkout's
    thirdparty). A misspelt flag is silently ignored by the game (LESSONS), so a flag outside this set is refused."""
    key = (str(repo), str(main))
    if key in _cvar_cache:
        return _cvar_cache[key]
    roots = [Path(repo) / 'src', Path(main) / 'src', Path(main) / 'thirdparty' / 'rexglue-sdk-src' / 'src',
             Path(main) / 'thirdparty' / 'rexglue-sdk-src' / 'include', Path(main) / 'probes']
    names = set()
    seen = set()
    for r in roots:
        if not r.is_dir() or r.resolve() in seen:
            continue
        seen.add(r.resolve())
        for p in r.rglob('*'):
            if p.suffix.lower() in ('.h', '.cpp', '.hpp', '.cc', '.inl') and p.is_file():
                try:
                    names.update(_DEFINE.findall(p.read_text(encoding='utf-8', errors='replace')))
                except OSError:
                    pass
    if len(names) < 300:
        raise RuntimeError(f'cvar index too small ({len(names)} names): the SDK source or src\\ was not found under {repo} / {main}; '
                           'refusing to judge flags against an empty list')
    names |= NON_CVAR_FLAGS
    _cvar_cache[key] = names
    return names


def load_launcher_flags(repo=REPO):
    p = Path(repo) / 'src' / 'launcher' / 'launcher_main.cpp'
    if not p.is_file():
        p = Path(MAIN) / 'src' / 'launcher' / 'launcher_main.cpp'
    txt = p.read_text(encoding='utf-8', errors='replace')
    found = set(re.findall(r'a == "(--[\w-]+)"', txt))
    if len(found) < 20:
        raise RuntimeError(f'launcher flag list too small ({len(found)}) from {p}')
    return found


# ------------------------------------------------------------------------------------------------------------------ plan
# Flags run_list.ps1 puts on the command itself (a second copy makes CLI11 drop every later startup flag: LESSONS "Shell, scripts and the harness").
RUN_LIST_OWNED = {'user_data_root', 'cache_root', 'wwe2k14_input_script'}
RUN_LIST_OWNED_TIMED = {'wwe2k14_timeline'}


@dataclass
class Finding:
    rule: int
    key: str
    severity: str     # 'refuse' | 'warn'
    msg: str
    why: str = ''


@dataclass
class Plan:
    kind: str                       # 'speed' | 'check' | 'picture'
    mode: str                       # 'game' | 'launcher'
    exe: Path
    route: Route
    caller_flags: list = field(default_factory=list)   # the command line's flags (Flag)
    door_flags: list = field(default_factory=list)    # flags the door itself adds (black box)
    launcher_args: list = field(default_factory=list)  # launcher mode: the launcher's own flags
    alive_games: int = 0
    arms: Optional[dict] = None     # picture: {'a': [Flag], 'b': [Flag], 'under_test': [names], 'route_b': Route|None}
    lint_only: bool = False         # --lint-all: no caller; rule 2's "pass --wwe2k14_launch" is then a note, not a fault
    main: Path = MAIN
    repo: Path = REPO
    bad_tokens: list = field(default_factory=list)
    exe_has_case: Optional[bool] = None

    def merged(self):
        """The game's flags as run_list will build them: the header's (unless the caller sets the same name), then the caller's, then the door's.
        Returns (flags, dropped_header)."""
        caller_names = {f.name for f in self.caller_flags + self.door_flags}
        flags, dropped = [], []
        for tok, src in self.route.header_flags:
            f = parse_flags([tok], src)[0][0]
            (dropped if f.name in caller_names else flags).append(f)
        return flags + list(self.caller_flags) + list(self.door_flags), dropped

    @property
    def timed(self):
        return self.kind == 'speed'


def _allowed(plan, key):
    return key in plan.route.allow


def _finding(plan, rule, key, sev, msg, why):
    if _allowed(plan, key):
        return Finding(rule, key, 'warn', f'[waived by the route: {plan.route.allow[key] or "no reason given"}] {msg}', why)
    return Finding(rule, key, sev, msg, why)


# --------------------------------------------------------------------------------------------------- rule 1: speed runs
# Verify / diagnostic switches: a timed number measured with one of these on is not the player's speed.
#   LESSONS "A verify / diagnostic switch left on invalidates an A/B (+25-36% in one arm)"; S100 "Read a route's header flags before reading
#   its speed" (Vulkan validation: 50-130 ms a frame vs 0.8 ms).
DIAG_RE = re.compile(r'(validation|verify|debug|diag|trace|capture|census|renderdoc|scene_records|dump|det_hash|det_serial|^vulkan_log_)', re.I)
STRICT_RE = re.compile(r'(validation|_verify)', re.I)   # the ones a route's own `cvar` step must not switch on in a timed run


def rule_1_speed(plan):
    out = []
    if plan.kind != 'speed':
        return out
    merged, _ = plan.merged()
    for f in merged:
        if STRICT_RE.search(f.bare) and f.truthy():
            out.append(_finding(plan, 1, 'speed', 'refuse',
                                f'a speed run has validation / verify on: {f.text()} ({f.source})',
                                'LESSONS S100 "Read a route\'s header flags before reading its speed": Vulkan validation made one scene 50-130 ms a frame instead of 0.8 ms; '
                                'a verify switch left on moved an A/B by 25-36% ([h2:66]).'))
        elif (f.bare == 'wwe2k14_scene_records' and f.truthy() and
              any(g.truthy() and re.match(r'wwe2k14_rdr_\w+_ours$', g.bare) for g in merged)):
            continue  # S101 T1: the promotions need the scene records: their cost is part of the promotion's own number, not a diagnostic
        elif DIAG_RE.search(f.bare) and f.truthy() and not f.bare.startswith(('wwe2k14_case',)):
            out.append(_finding(plan, 1, 'speed', 'refuse',
                                f'a speed run has a diagnostic switch on: {f.text()} ({f.source})',
                                'LESSONS "A verify / diagnostic switch left on invalidates an A/B": time a clean build; diagnose in a separate check run.'))
    for st in plan.route.steps:
        if st.cmd == 'cvar':
            nm, _, val = st.args.partition(' ')
            if STRICT_RE.search(nm) and val.strip().lower() not in FALSY:
                out.append(_finding(plan, 1, 'speed', 'refuse',
                                    f'the route switches {nm} on at {st.file}:{st.line}, inside a speed run',
                                    'LESSONS S100 (validation in a timed run) / [h2:66] (verify left on).'))
    if plan.alive_games > 0:
        out.append(_finding(plan, 1, 'speed', 'refuse', f'{plan.alive_games} other game(s) alive: a timed run never runs beside another game',
                            'CLAUDE.md rule 7 / run_list.ps1 -Exclusive; LESSONS "GPU ms compare only at the same clock state and background load" (lanes share the GPU).'))
    vs = [f for f in merged if f.bare == 'vsync']
    if any(f.value is not None and f.value.lower() in FALSY for f in vs) and not plan.route.in_run_ab:
        out.append(_finding(plan, 1, 'speed', 'refuse',
                            'a speed run is fast-forwarded (--vsync=false) and the route does not say it is an in-run A/B',
                            'LESSONS S82: a fast-forwarded in-run A/B overstated Bryan\'s gain 3-4x against normal speed; speed numbers come from normal-speed runs. '
                            'In-run A/B routes (name ab_*, or "in-run A/B" in the header) may fast-forward.'))
    return out


# --------------------------------------------------------------------------------------------------- rule 2: launch route
def rule_2_launch(plan):
    out = []
    r = plan.route
    if not r.needs_launch:
        return out
    merged, _ = plan.merged()
    launch = [f for f in merged if f.bare == 'wwe2k14_launch']
    if plan.mode == 'launcher':
        # a launcher run builds its own game flags (--launch / --scenario); the route's launch note is the launcher's job
        return out
    if not launch:
        sev = 'warn' if plan.lint_only else 'refuse'
        note = ' (lint: the caller must pass it)' if plan.lint_only else ''
        out.append(_finding(plan, 2, 'launch', sev,
                            f'{r.name} names a launch scenario in its header{(" (" + r.launch_scenario + ")") if r.launch_scenario else ""} but no --wwe2k14_launch= flag is given{note}',
                            'LESSONS S93 "Read a route\'s header before a timed run": profile_lesnar_bryan.txt needs --wwe2k14_launch=lesnar_bryan (2 wasted timed runs); '
                            'run_list.ps1 (Session 14, runs 1024-1027) sat at the main menu.'))
    else:
        for f in launch:
            if f.value is None or not f.value.strip():
                out.append(_finding(plan, 2, 'launch', 'refuse', f'{f.text()} has no scenario name ({f.source})', 'run_list.ps1: an empty flag value drops the other flags (runs 1093/1094).'))
            elif r.launch_scenario and f.value != r.launch_scenario:
                out.append(Finding(2, 'launch', 'warn', f'the route\'s header says {r.launch_scenario}, the run launches {f.value} ({f.source})', 'check this is intended.'))
    return out


# ---------------------------------------------------------------------------------------------------- rule 3: the flags
def rule_3_flags(plan):
    out = []
    cvars = load_cvars(plan.repo, plan.main)
    merged, dropped = plan.merged()
    for t in plan.bad_tokens:
        out.append(Finding(3, 'flags', 'refuse', f'"{t}" is not a --flag=value token', 'write values as --flag=value; a loose word is not passed on (CLI11).'))
    seen = {}
    own = RUN_LIST_OWNED | (RUN_LIST_OWNED_TIMED if plan.timed else set())
    for f in merged:
        if f.bare not in cvars:
            near = difflib.get_close_matches(f.bare, cvars, n=2, cutoff=0.8)
            out.append(_finding(plan, 3, 'flags', 'refuse', f'unknown flag {f.text()} ({f.source})' + (f'; did you mean {", ".join("--" + n for n in near)}?' if near else ''),
                                'LESSONS: a misspelt flag is silently ignored by the game; the index is every REXCVAR_DEFINE_* in src\\ and the SDK.'))
        if f.value is not None and f.value == '':
            out.append(Finding(3, 'flags', 'refuse', f'empty value {f.name}= ({f.source})', 'run_list.ps1 Session 15: an empty value made the game start without its input script.'))
        if f.bare in own:
            out.append(Finding(3, 'flags', 'refuse', f'{f.name} is set by run_list itself; {f.source} would pass it twice', 'a flag given twice makes CLI11 drop later flags (LESSONS).'))
        if f.name in seen:
            out.append(Finding(3, 'flags', 'refuse', f'{f.name} appears twice: {seen[f.name].text()} ({seen[f.name].source}) and {f.text()} ({f.source})',
                               'LESSONS "Shell, scripts and the harness": a flag passed twice makes CLI11 drop every later startup flag.'))
        seen[f.name] = f
    for n in plan.route.header_dup_names:
        out.append(Finding(3, 'flags', 'warn', f'{plan.route.name}: header gives {n} more than once (run_list keeps the last)', 'cosmetic; run_list de-duplicates.'))
    for t in plan.route.header_not_flags:
        out.append(Finding(3, 'flags', 'warn', f'{plan.route.name}: header token "{t}" is not a --flag (run_list ignores it)', 'a "--flag value" pair in a header loses its value.'))
    if plan.mode == 'launcher':
        lf = load_launcher_flags(plan.repo)
        lseen = set()
        for f in plan.launcher_args:
            if f.name not in lf:
                near = difflib.get_close_matches(f.name, lf, n=2, cutoff=0.8)
                out.append(Finding(3, 'flags', 'refuse', f'unknown launcher flag {f.text()}' + (f'; did you mean {", ".join(near)}?' if near else ''), 'launcher_main.cpp ParseArgs is the list.'))
            if f.name in lseen and f.name not in ('--assign', '--select'):
                out.append(Finding(3, 'flags', 'refuse', f'launcher flag {f.name} appears twice', 'a repeated flag is dropped or doubled.'))
            lseen.add(f.name)
    return out


# ----------------------------------------------------------------------------------------------- rule 4: settings, no-save
def _under_out(value, plan):
    if not value:
        return False
    p = Path(value)
    for base in (Path(plan.main), Path(plan.repo)):
        q = p if p.is_absolute() else base / p
        try:
            q.resolve().relative_to((base / 'out').resolve())
            return True
        except ValueError:
            continue
    return False


def _is_dev_exe(exe, plan):
    s = str(exe).lower().replace('/', '\\')
    return '\\out\\build\\' in s or '\\thirdparty\\' in s or '\\.claude\\worktrees\\' in s


def rule_4_settings(plan):
    out = []
    if plan.mode == 'launcher':
        names = {f.name for f in plan.launcher_args}
        missing = [n for n in ('--exit', '--no-save') if n not in names]
        if missing:
            out.append(_finding(plan, 4, 'settings', 'refuse', f'a launcher run is missing {" and ".join(missing)}',
                                'CLAUDE.md Safety / run_list S95: a launcher with no --exit held the run lock 3 h 51 min; the launcher rewrites its settings file '
                                'unless --no-save (LESSONS S95 "The launcher rewrites its --settings file").'))
        return out
    merged, _ = plan.merged()
    byname = {f.bare: f for f in merged}
    reasons = []
    uls = byname.get('wwe2k14_use_launcher_settings')
    if uls and uls.truthy():
        reasons.append('--wwe2k14_use_launcher_settings is on')
    if not _is_dev_exe(plan.exe, plan):
        reasons.append('the exe is a release build (it reads the launcher settings by default)')
    if plan.route.touches_gfx_menu:
        reasons.append('the route goes through the in-game GRAPHICS menu (it writes the settings file)')
    if reasons:
        sf = byname.get('wwe2k14_settings_file')
        sev = 'warn' if plan.lint_only else 'refuse'
        if not sf or not _under_out(sf.value, plan):
            out.append(_finding(plan, 4, 'settings', sev,
                                'this run can touch the player\'s settings (' + '; '.join(reasons) + ') but --wwe2k14_settings_file=<a file under out\\> is not given'
                                + (f' (given: {sf.text()})' if sf else '') + (' (lint: the caller must pass it)' if plan.lint_only else ''),
                                'CLAUDE.md Safety: test runs never read or write Jarrod\'s %APPDATA%\\wwe2k14-launcher\\settings.json.'))
        if not _is_dev_exe(plan.exe, plan):
            rd = byname.get('wwe2k14_replay_dir')
            if not rd or not _under_out(rd.value, plan):
                out.append(_finding(plan, 4, 'settings', 'refuse', 'a release exe needs --wwe2k14_replay_dir=<under out\\> too',
                                    'CLAUDE.md Safety (a release exe also --wwe2k14_replay_dir).'))
    return out


# ------------------------------------------------------------------------------------------- rule 5: shots wait on state
def _fast_forwarded(plan):
    if plan.kind == 'speed' or plan.route.has_realtime:
        return False
    merged, _ = plan.merged()
    return not any(f.bare == 'vsync' for f in merged) or any(f.bare == 'vsync' and f.value and f.value.lower() in FALSY for f in merged)


def rule_5_shots(plan):
    out = []
    r = plan.route
    if not r.is_entrance or not _fast_forwarded(plan):
        return out
    pending = None   # (file, line) of the first wait_frames since the last state sync
    for st in r.steps:
        if st.cmd in STATE_SYNC_STEPS:
            pending = None
        elif st.cmd == 'wait_frames' and pending is None:
            pending = (st.file, st.line)
        elif st.cmd in SHOT_STEPS and pending is not None:
            if st.cmd == 'screenshot_every' and st.args.split()[:1] == ['0']:
                continue
            out.append(_finding(plan, 5, 'shots', 'refuse',
                                f'{st.cmd} at {st.file}:{st.line} follows wait_frames at {pending[0]}:{pending[1]} in an entrance run that is fast-forwarded',
                                'LESSONS S100 "A fast-forwarded entrance shot follows the wall clock": entrances sync to the video / music, so a frame count after a sync '
                                'lands on a different moment when the speed changes (gap 45 vs 2). Sync the shot on game state (wait_state / wait_text) with no wait_frames '
                                'in between, or make the route realtime (`# harness: realtime`).'))
            pending = None   # one finding per shot
    return out


# -------------------------------------------------------------------------------------------------- rule 6: picture A/B
# Per-run plumbing that legitimately differs between two runs of the same picture test (they never change the look).
LOOK_NEUTRAL = {'wwe2k14_case', 'wwe2k14_case_dir', 'user_data_root', 'cache_root', 'wwe2k14_input_script', 'wwe2k14_rdr_live_picture',
                'wwe2k14_rdr_live_png', 'wwe2k14_rdr_capture_dir', 'wwe2k14_rdr_capture_label', 'audio_mute'}


def rule_6_picture(plan):
    out = []
    if plan.kind != 'picture':
        return out
    a = plan.arms
    if not a:
        return [_finding(plan, 6, 'picture', 'refuse', 'a picture A/B needs both arms: --arm-a "FLAGS" --arm-b "FLAGS" --under-test NAME', 'LESSONS S90 (Bryan\'s FAIL).')]
    ut = {n.lstrip('-') for n in a.get('under_test', [])}
    if not ut:
        out.append(_finding(plan, 6, 'picture', 'refuse', 'a picture A/B must name the switch under test (--under-test NAME)', 'without it "equal except one switch" cannot be checked.'))

    def lookmap(flags, route):
        m = {}
        for tok, _src in route.header_flags:
            f = parse_flags([tok], 'header')[0][0]
            m[f.bare] = f.value
        for f in flags:
            m[f.bare] = f.value
        return {k: v for k, v in m.items() if k not in LOOK_NEUTRAL}

    ra = plan.route
    rb = a.get('route_b') or plan.route
    ma, mb = lookmap(a['a'], ra), lookmap(a['b'], rb)
    diff = sorted(k for k in set(ma) | set(mb) if ma.get(k, '<unset>') != mb.get(k, '<unset>'))
    extra = [k for k in diff if k not in ut]
    if extra:
        det = ', '.join(f'--{k}: A={ma.get(k, "<unset>")} B={mb.get(k, "<unset>")}' for k in extra)
        out.append(_finding(plan, 6, 'picture', 'refuse', f'the arms differ in more than the switch under test: {det}',
                            'LESSONS S90 (Bryan\'s FAIL): the look settings of both arms must be equal except the one under test, or the picture difference is not the switch.'))
    if ut and not any(k in ut for k in diff):
        out.append(_finding(plan, 6, 'picture', 'refuse', f'the arms do not differ in --{", --".join(sorted(ut))}: nothing is under test', 'an A/A is not an A/B.'))
    if a.get('route_b') is not None and a['route_b'].path != ra.path:
        out.append(Finding(6, 'picture', 'warn', f'the arms use different routes ({ra.name} / {a["route_b"].name}): their cvar steps are not compared', 'check the look steps by eye.'))
    return out


RULES = [
    (1, 'speed', rule_1_speed),
    (2, 'launch', rule_2_launch),
    (3, 'flags', rule_3_flags),
    (4, 'settings', rule_4_settings),
    (5, 'shots', rule_5_shots),
    (6, 'picture', rule_6_picture),
]


def arm_plans(plan):
    """A picture plan runs as two ordinary runs: each arm is a 'check' plan (same route, the common flags + the arm's own)."""
    import dataclasses
    res = []
    for label in ('a', 'b'):
        rt = (plan.arms.get('route_b') if label == 'b' else None) or plan.route
        res.append((label.upper(), dataclasses.replace(plan, kind='check', route=rt, caller_flags=list(plan.caller_flags) + list(plan.arms[label]), arms=None)))
    return res


def check_plan(plan):
    out = []
    if plan.kind == 'picture':
        out.extend(rule_6_picture(plan))
        if plan.arms:
            for label, sub in arm_plans(plan):
                for n, _k, fn in RULES:
                    if n != 6:
                        for f in fn(sub):
                            f.msg = f'[arm {label}] {f.msg}'
                            out.append(f)
        return out
    for _n, _k, fn in RULES:
        out.extend(fn(plan))
    return out


def refused(findings):
    return [f for f in findings if f.severity == 'refuse']
