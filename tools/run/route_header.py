"""Session 101 (arc A4, the run contract): read a route (an input script) the way tools\\bench\\run_list.ps1 and the game do.

Facts this file encodes (each one is read from run_list.ps1 / src\\input_script.h, not guessed):
  - The header is the first 40 lines of the top file. `# harness: flags --a=1 --b=2` (several lines add up) are flags run_list puts in
    front of the route's -ScriptExtras entry; `# harness: realtime (reason)` = normal speed; `# harness: step30`.
  - run_list refuses a route whose first 16 lines say `Use with --wwe2k14_launch` when no --wwe2k14_launch= flag is in -Extra / the header's flags.
  - `include FILE` is looked up beside the script, then tools\\input_scripts\\nav\\, then tools\\input_scripts\\ (walking up); up to 8 deep.
  - Steps are one per line: `cmd args`; `#` starts a comment line.
Import-only (no side effects). Used by contract.py and run.py.
"""
import re
from dataclasses import dataclass, field
from pathlib import Path

HEADER_LINES = 40          # run_list: Get-Content -TotalCount 40
LAUNCH_NOTE_LINES = 16     # run_list: the "Use with --wwe2k14_launch" check reads 16
MAX_INCLUDE_DEPTH = 8      # input_script.h: "Up to 8 levels deep, no loops"

# Steps that wait on game STATE (not on the wall clock or a frame count): a shot taken right after one of these is exact.
STATE_SYNC_STEPS = {'wait_state', 'wait_text', 'press_until_state', 'press_until_text', 'wait_file', 'press_until_file'}
SHOT_STEPS = {'screenshot', 'screenshot_every'}

_HARNESS_FLAGS = re.compile(r'^#\s*harness:\s*flags\s+(.+)$')
_HARNESS_REALTIME = re.compile(r'^#\s*harness:\s*realtime\b(.*)$')
_HARNESS_STEP30 = re.compile(r'^#\s*harness:\s*step30\b')
_CONTRACT_ALLOW = re.compile(r'^#\s*contract:\s*allow\s+([\w,-]+)\s*(.*)$')
_LAUNCH_NEEDED = re.compile(r'(Use with|Needs?|Run with|Requires?|Use it with)\s+[^\n]*?--wwe2k14_launch\b(?:=([\w-]+))?', re.I)


@dataclass
class Step:
    cmd: str
    args: str
    file: str
    line: int


@dataclass
class Route:
    path: Path
    name: str
    lines: int = 0
    header_flags: list = field(default_factory=list)      # [(token, 'route header line N')], run_list order (last of a name wins)
    header_dup_names: list = field(default_factory=list)  # names given twice in the header (run_list keeps the last)
    header_not_flags: list = field(default_factory=list)  # header tokens that are not --flags (run_list ignores them)
    realtime: str = ''                                    # the reason text when the header says realtime, else ''
    has_realtime: bool = False
    step30: bool = False
    needs_launch: bool = False
    launch_scenario: str = ''                             # the NAME the note gives, '' when none / a placeholder
    allow: dict = field(default_factory=dict)             # `# contract: allow RULE reason` -> {rule: reason}
    in_run_ab: bool = False                               # the route says it is an in-run A/B (rule 1's fast-forward exception)
    steps: list = field(default_factory=list)
    unresolved_includes: list = field(default_factory=list)
    includes: list = field(default_factory=list)
    is_entrance: bool = False                             # waits on an entrance (a stage video, entrance_step_*)
    touches_gfx_menu: bool = False                        # presses through the in-game GRAPHICS menu (can write the settings file)
    header_text: str = ''


def _resolve_include(name, here, input_root):
    cands = []
    nm = name if name.lower().endswith('.txt') else name + '.txt'
    cands.append(here / nm)
    cands.append(input_root / 'nav' / nm)
    cands.append(input_root / nm)
    d = here
    while True:  # walking up from the script
        cands.append(d / nm)
        if d == input_root or d.parent == d:
            break
        d = d.parent
    for c in cands:
        if c.is_file():
            return c
    return None


def _expand(path, input_root, depth, route, stack):
    try:
        raw = path.read_text(encoding='utf-8', errors='replace').splitlines()
    except OSError as e:
        raise FileNotFoundError(f'cannot read route {path}: {e}')
    out = []
    for i, text in enumerate(raw, 1):
        s = text.strip()
        if not s or s.startswith('#'):
            continue
        cmd, _, rest = s.partition(' ')
        if cmd == 'include':
            if depth >= MAX_INCLUDE_DEPTH:
                route.unresolved_includes.append(f'{path.name}:{i} include too deep')
                continue
            target = _resolve_include(rest.strip(), path.parent, input_root)
            if target is None or target in stack:
                route.unresolved_includes.append(f'{path.name}:{i} include {rest.strip()}')
                continue
            route.includes.append(target.name)
            out.extend(_expand(target, input_root, depth + 1, route, stack | {target}))
            continue
        out.append(Step(cmd, rest.strip(), path.name, i))
    return out


def find_route(arg, repo_root, exe_dir=None):
    """Resolve a route argument the way run_list does: as given, else relative to the repo, else to the exe folder."""
    p = Path(arg)
    for c in ([p] if p.is_absolute() else [Path.cwd() / p, Path(repo_root) / p] + ([Path(exe_dir) / p] if exe_dir else [])):
        if c.is_file():
            return c.resolve()
    raise FileNotFoundError(f'input script not found: {arg}')


def parse_route(path, repo_root):
    path = Path(path).resolve()
    input_root = Path(repo_root) / 'tools' / 'input_scripts'
    if not input_root.is_dir():
        input_root = path.parent
    route = Route(path=path, name=path.name)
    text = path.read_text(encoding='utf-8', errors='replace')
    all_lines = text.splitlines()
    route.lines = len(all_lines)
    head = all_lines[:HEADER_LINES]
    route.header_text = '\n'.join(head)
    toks = {}
    order = []
    for n, ln in enumerate(head, 1):
        s = ln.strip()
        m = _HARNESS_FLAGS.match(s)
        if m:
            for tok in m.group(1).split():
                if not re.match(r'^--[\w.]+', tok):
                    route.header_not_flags.append(tok)
                    continue
                name = tok.split('=', 1)[0]
                if name in toks:
                    route.header_dup_names.append(name)
                toks[name] = (tok, f'route header line {n}')
        m = _HARNESS_REALTIME.match(s)
        if m and not route.has_realtime:
            route.has_realtime = True
            route.realtime = m.group(1).strip()
        if _HARNESS_STEP30.match(s):
            route.step30 = True
        m = _CONTRACT_ALLOW.match(s)
        if m:
            for rule in m.group(1).split(','):
                route.allow[rule.strip().lower()] = m.group(2).strip()
    route.header_flags = list(toks.values())
    for ln in all_lines[:LAUNCH_NOTE_LINES]:
        if ln.lstrip().startswith('#') and re.search(r'Use with[^\n]*--wwe2k14_launch\b', ln):
            route.needs_launch = True
    for m in _LAUNCH_NEEDED.finditer('\n'.join(l for l in head if l.lstrip().startswith('#'))):
        route.needs_launch = True
        nm = m.group(2) or ''
        if nm and nm.upper() not in ('NAME', 'X', 'SCENARIO') and not route.launch_scenario:
            route.launch_scenario = nm
    low = route.header_text.lower()
    route.in_run_ab = bool(re.search(r'in-run a/b|paired a/b|in-run ab\b', low)) or path.name.startswith('ab_')
    route.steps = _expand(path, input_root, 0, route, {path})
    for st in route.steps:
        a = st.args.lower()
        if st.cmd == 'wait_file' and '.bik' in a:
            route.is_entrance = True
        if st.cmd in ('wait_state', 'press_until_state', 'assert_state') and 'entrance' in a:
            route.is_entrance = True
        if st.cmd in ('wait_text', 'assert_text', 'press_until_text') and 'graphics' in a:
            route.touches_gfx_menu = True
    if 'entrance' in path.name.lower():
        route.is_entrance = True
    if 'gfx' in path.name.lower() and 'menu' in route.header_text.lower():
        route.touches_gfx_menu = True
    return route
