"""Session 101 (arc A4, tool 8): the door for every game run.

  py -I tools\\run\\run.py ROUTE [--speed | --picture | --check] [--wwe2k14_x=1 ...] [--dry-run]
  py -I tools\\run\\run.py ROUTE --launcher --exit --no-save [launcher flags]        (a launcher run: the launcher's own flags)
  py -I tools\\run\\run.py ROUTE --picture --arm-a "--x=0" --arm-b "--x=1" --under-test x   (a picture A/B: two runs)
  py -I tools\\run\\run.py --lint-all                                                  (every route; out\\s101\\route_lint.md)

Prints the plan (exe, route, every flag and where it came from, what run_list adds), runs the contract (contract.py: six rules, each with
its reason), refuses contradictions (exit 2), then starts the run through tools\\bench\\run_list.ps1 (the run lock, the frozen user data, the
triage): hidden and at low priority for untimed runs (a timed --speed run keeps normal priority: its numbers must be the player's), with a
hard timeout and a heartbeat line every 60 s, and a black box: --wwe2k14_case=true --wwe2k14_case_dir=<out\\s101\\runs\\STAMP_ROUTE> so the
case hook (src\\case\\case_hook.h; triggers RunExit / RunShot / RunFailure) leaves a case zip there. Kinds: --check (default, untimed, fast-forward),
--speed (timed: run_list -Exclusive, normal speed), --picture (untimed A/B of two arms). Flags are --name=value (a loose word is refused).
Unknown options are the game's flags (or the launcher's with --launcher). Exit: 0 run ok, 1 run failed / timed out, 2 refused.
"""
import argparse
import datetime
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import contract as C  # noqa: E402
from route_header import find_route, parse_route  # noqa: E402

REPO, MAIN = C.REPO, C.MAIN
RUN_LIST = REPO / 'tools' / 'bench' / 'run_list.ps1'
RUNLOCK = REPO / 'tools' / 'runlock.ps1'
DEFAULT_EXE = MAIN / 'out' / 'build' / 'win-amd64-release' / 'wwe2k14.exe'
DEFAULT_LAUNCHER = MAIN / 'out' / 'build' / 'win-amd64-release' / 'wwe2k14_launcher.exe'
RUNS_DIR = MAIN / 'out' / 's101' / 'runs'
CREATE_NO_WINDOW = 0x08000000
BELOW_NORMAL = 0x00004000
CASE_MARK = b'wwe2k14_case_dir'


def now_stamp():
    return datetime.datetime.now().strftime('%Y%m%d_%H%M%S')


def exe_has_case_hook(exe):
    """True/False when the exe file can be read (its string table holds the case hook's cvar name), None when it does not exist."""
    exe = Path(exe)
    if not exe.is_file():
        return None
    key = (str(exe), exe.stat().st_mtime_ns, exe.stat().st_size)
    if key in _case_cache:
        return _case_cache[key]
    _case_cache[key] = _scan_exe(exe)
    return _case_cache[key]


_case_cache = {}


def _scan_exe(exe):
    try:
        tail = b''
        with open(exe, 'rb') as fh:
            while True:
                buf = fh.read(8 << 20)
                if not buf:
                    return False
                if CASE_MARK in tail + buf[:len(CASE_MARK)] or CASE_MARK in buf:
                    return True
                tail = buf[-len(CASE_MARK):]
    except OSError:
        return None


def alive_game_count():
    try:
        r = subprocess.run(['tasklist', '/FO', 'CSV', '/NH', '/FI', 'IMAGENAME eq wwe2k14.exe'], capture_output=True, text=True, timeout=30)
        return sum(1 for ln in r.stdout.splitlines() if ln.lower().startswith('"wwe2k14.exe"'))
    except Exception:
        return 0


def make_parser():
    ap = argparse.ArgumentParser(prog='run.py', description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter, allow_abbrev=False)
    ap.add_argument('route', nargs='?', help='a route (input script): full path, repo-relative, or relative to the exe folder')
    k = ap.add_mutually_exclusive_group()
    k.add_argument('--speed', dest='kind', action='store_const', const='speed', help='a timed run (run_list -Exclusive, normal speed, normal priority)')
    k.add_argument('--picture', dest='kind', action='store_const', const='picture', help='a picture A/B: two arms (--arm-a / --arm-b / --under-test)')
    k.add_argument('--check', dest='kind', action='store_const', const='check', help='an ordinary untimed check (default)')
    ap.add_argument('--dry-run', action='store_true', help='print the plan and the verdict; start nothing')
    ap.add_argument('--lint-all', action='store_true', help='run the rules over every route in tools\\input_scripts; write out\\s101\\route_lint.md')
    ap.add_argument('--lint-out', default=str(MAIN / 'out' / 's101' / 'route_lint.md'))
    ap.add_argument('--launcher', action='store_true', help='start the launcher (--harness ROUTE) instead of the game; other flags are the launcher\'s')
    ap.add_argument('--exe', default=str(DEFAULT_EXE), help='the game exe (or, with --launcher, the launcher exe when it ends in launcher.exe)')
    ap.add_argument('--timeout', type=int, default=720, help='run_list -WaitSeconds; the door kills the whole tree 240 s after it')
    ap.add_argument('--arm-a', default='', help='picture A/B: flags of arm A, one quoted string')
    ap.add_argument('--arm-b', default='', help='picture A/B: flags of arm B')
    ap.add_argument('--route-b', default='', help='picture A/B: arm B\'s route when it differs')
    ap.add_argument('--under-test', default='', help='picture A/B: the flag name(s) that differ on purpose, comma separated')
    ap.add_argument('--no-defaults', action='store_true', help='(tests) do not add the launcher defaults / the black box')
    ap.add_argument('--alive-games', type=int, default=-1, help='(tests) pretend N other games are alive; default: count wwe2k14.exe')
    return ap


def build_plan(ns, rest, dry=False):
    exe = Path(ns.exe)
    route_path = find_route(ns.route, REPO, exe.parent)
    route = parse_route(route_path, REPO)
    kind = ns.kind or 'check'
    mode = 'launcher' if ns.launcher else 'game'
    plan = C.Plan(kind=kind, mode=mode, exe=exe, route=route, main=MAIN, repo=REPO)
    flags, bad = C.parse_flags(rest, 'command line')
    plan.bad_tokens = bad
    if mode == 'launcher':
        plan.launcher_args = flags
        if not ns.no_defaults:
            have = {f.name for f in flags}
            for n in ('--exit', '--no-save'):
                if n not in have:
                    plan.launcher_args.append(C.Flag(n, None, 'door: launcher default', n))
        if exe.name.lower() == 'wwe2k14.exe':
            plan.exe = DEFAULT_LAUNCHER
    else:
        plan.caller_flags = flags
    if kind == 'picture':
        a, ba = C.parse_flags(ns.arm_a, 'arm A')
        b, bb = C.parse_flags(ns.arm_b, 'arm B')
        plan.bad_tokens += ba + bb
        rb = parse_route(find_route(ns.route_b, REPO, exe.parent), REPO) if ns.route_b else None
        plan.arms = {'a': a, 'b': b, 'under_test': [x for x in ns.under_test.split(',') if x], 'route_b': rb}
    plan.alive_games = ns.alive_games if ns.alive_games >= 0 else alive_game_count()
    if mode == 'game':
        plan.exe_has_case = exe_has_case_hook(plan.exe)
        if plan.exe_has_case and not ns.no_defaults:
            have = {f.name for f in plan.caller_flags}
            plan.stamp = now_stamp()     # lane 294: ONE stamp for the case dir and the record dir (two stamps made two run folders a second apart)
            rd = RUNS_DIR / f'{plan.stamp}_{route.path.stem}'
            if '--wwe2k14_case' not in have:
                plan.door_flags.append(C.Flag('--wwe2k14_case', 'true', 'door: black box'))
            if '--wwe2k14_case_dir' not in have:
                plan.door_flags.append(C.Flag('--wwe2k14_case_dir', str(rd), 'door: black box'))
    return plan


def speed_description(plan):
    """What run_list will do about speed (its Get-RunListRealTimeReason), for the plan print."""
    merged, _ = plan.merged()
    if plan.kind == 'speed':
        return 'normal speed (run_list -Exclusive: a timed run)'
    if plan.route.has_realtime:
        return f'normal speed (route header: realtime {plan.route.realtime})'
    if any(f.bare == 'vsync' for f in merged):
        return 'as the flags set --vsync'
    return 'fast-forward (run_list adds --vsync=false --audio_mute=true)'


def print_plan(plan, findings, out=None):
    p = lambda s='': print(s, file=out or sys.stdout)  # noqa: E731
    r = plan.route
    p(f'RUN PLAN  kind={plan.kind}  mode={plan.mode}')
    p(f'  exe     : {plan.exe}' + ('' if plan.mode == 'launcher' else {True: '  (case hook present)', False: '  (NO case hook in this exe: no black box)', None: '  (exe not found: case hook unknown)'}[plan.exe_has_case]))
    p(f'  route   : {r.path}  ({len(r.steps)} steps, {r.lines} lines' + (f', includes {", ".join(sorted(set(r.includes)))}' if r.includes else '') + ')'
      + ('  [entrance]' if r.is_entrance else '') + ('  [GRAPHICS menu]' if r.touches_gfx_menu else ''))
    if r.allow:
        p('  waivers : ' + '; '.join(f'{k} ({v or "no reason"})' for k, v in r.allow.items()))
    if plan.mode == 'launcher':
        p(f'  starts  : launcher --harness {r.path}  (inside `runlock.ps1 run`, exclusive)')
        p('  launcher flags:')
        for f in [C.Flag('--harness', str(r.path), 'door')] + plan.launcher_args:
            p(f'    {f.text():48} [{f.source}]')
    else:
        p(f'  speed   : {speed_description(plan)}')
        p('  priority: ' + ('normal (a timed run)' if plan.timed else 'hidden window, below-normal priority (run_list keeps the game window behind others)'))
        p(f'  starts  : tools\\bench\\run_list.ps1 -Scripts <route> -Extra <flags below that the caller/door set>{" -Exclusive" if plan.timed else ""} -WaitSeconds N')
        merged, dropped = plan.merged()
        p('  flags (in the order run_list builds them):')
        p(f'    {"--user_data_root/--cache_root/--wwe2k14_input_script":48} [run_list: its own work copy of the frozen user data]')
        for f in merged:
            p(f'    {f.text():48} [{f.source}]')
        for f in dropped:
            p(f'    {f.text():48} [{f.source}] left out: the caller sets {f.name}')
        if not merged and not dropped:
            p('    (no further flags)')
        if plan.kind == 'picture' and plan.arms:
            for lab, sub in C.arm_plans(plan):
                p(f'  arm {lab}: ' + (' '.join(f.text() for f in plan.arms[lab.lower()]) or '(no own flags)')
                  + (f'  route {sub.route.name}' if sub.route.path != r.path else ''))
            p(f'  under test: {", ".join(plan.arms["under_test"]) or "(none named)"}')
    p(f'  other games alive: {plan.alive_games}')
    ref = C.refused(findings)
    warns = [f for f in findings if f.severity == 'warn']
    p()
    for f in warns:
        p(f'  WARN    rule {f.rule} ({f.key}): {f.msg}')
    for f in ref:
        p(f'  REFUSED rule {f.rule} ({f.key}): {f.msg}')
        if f.why:
            p(f'          why: {f.why}')
    p(f'VERDICT: {"REFUSED (" + str(len(ref)) + ")" if ref else "OK"}' + (f', {len(warns)} warning(s)' if warns else ''))


def _q(s):
    if "'" in s:
        raise ValueError(f'a single quote in {s!r}: not passed through PowerShell by the door')
    return "'" + s + "'"


def game_command(plan, route_path, extra_flags, timeout):
    """The pwsh command string: call run_list in-process with & (LESSONS: `pwsh -File` mangles -Extra)."""
    extra = ' '.join(f.text() for f in extra_flags)
    cmd = f"& {_q(str(RUN_LIST))} -Scripts {_q(str(route_path))} -Exe {_q(str(plan.exe))} -WaitSeconds {int(timeout)}"
    if extra:
        cmd += f' -Extra {_q(extra)}'
    if plan.timed:
        cmd += ' -Exclusive'
    return ['pwsh', '-NoProfile', '-NonInteractive', '-Command', cmd]


def launcher_command(plan):
    args = ['--harness', str(plan.route.path)] + [f.text() for f in plan.launcher_args]
    if any(' ' in a or "'" in a for a in args):
        raise ValueError('launcher arguments with spaces are not passed by the door (Start-Process splits them: LESSONS S100)')
    inner = f"Start-Process -Wait -FilePath {_q(str(plan.exe))} -ArgumentList {_q(' '.join(args))}"
    return ['pwsh', '-NoProfile', '-File', str(RUNLOCK), 'run', '-Name', f'door launcher {plan.route.name}', '--', 'pwsh', '-NoProfile', '-Command', inner]


def start_run(argv, log_path, timed, hard_timeout):
    """The ONE place a game / launcher process is started. Hidden; below-normal priority unless timed; killed (whole tree) at hard_timeout."""
    log_path.parent.mkdir(parents=True, exist_ok=True)
    flags = CREATE_NO_WINDOW | (0 if timed else BELOW_NORMAL)
    with open(log_path, 'w', encoding='utf-8', errors='replace') as lf:
        p = subprocess.Popen(argv, stdout=lf, stderr=subprocess.STDOUT, creationflags=flags, cwd=str(REPO))
        t0 = time.time()
        last_beat = t0
        while True:
            try:
                rc = p.wait(timeout=10)
                return rc, False
            except subprocess.TimeoutExpired:
                pass
            el = time.time() - t0
            if time.time() - last_beat >= 60:
                last_beat = time.time()
                lf.flush()
                tail = _last_line(log_path)
                print(f'[run] {int(el)} s, log {log_path.stat().st_size} bytes, last: {tail}', flush=True)
            if el > hard_timeout:
                subprocess.run(['taskkill', '/PID', str(p.pid), '/T', '/F'], capture_output=True)
                p.wait()
                return 1, True


def _last_line(path):
    try:
        lines = Path(path).read_text(encoding='utf-8', errors='replace').strip().splitlines()
        return (lines[-1] if lines else '')[:140]
    except OSError:
        return ''


def case_zips(d):
    d = Path(d)
    return sorted(d.glob('*.zip')) if d.is_dir() else []


def execute(plan, ns):
    stamp = getattr(plan, 'stamp', None) or now_stamp()
    rd = RUNS_DIR / f'{stamp}_{plan.route.path.stem}'
    rd.mkdir(parents=True, exist_ok=True)
    (rd / 'plan.json').write_text(json.dumps({
        'kind': plan.kind, 'mode': plan.mode, 'exe': str(plan.exe), 'route': str(plan.route.path),
        'flags': [f'{f.text()}  [{f.source}]' for f in plan.merged()[0]], 'launcher_args': [f.text() for f in plan.launcher_args]}, indent=1), encoding='utf-8')
    hard = ns.timeout + 240
    results = []
    if plan.mode == 'launcher':
        runs = [('launcher', launcher_command(plan), plan)]
    elif plan.kind == 'picture':
        runs = []
        for lab, sub in C.arm_plans(plan):
            extra = list(sub.caller_flags) + list(plan.door_flags)
            runs.append((f'arm {lab}', game_command(sub, sub.route.path, extra, ns.timeout), sub))
    else:
        runs = [('run', game_command(plan, plan.route.path, list(plan.caller_flags) + list(plan.door_flags), ns.timeout), plan)]
    rc_all = 0
    for label, argv, sub in runs:
        lp = rd / f'{label.replace(" ", "_")}.log'
        print(f'[run] starting {label}: log {lp}', flush=True)
        rc, killed = start_run(argv, lp, sub.timed, hard)
        text = lp.read_text(encoding='utf-8', errors='replace')
        res_lines = [ln for ln in text.splitlines() if '->' in ln and ('EXITED' in ln or 'KILLED' in ln or 'STILL' in ln or 'ABORT' in ln)]
        ok = (rc == 0 and not killed and bool(res_lines) and all('exit code 0' in ln and 'FAILED' not in ln for ln in res_lines)) if plan.mode == 'game' else (rc == 0 and not killed)
        print(f'[run] {label}: {"killed at the hard timeout" if killed else "ended"}, process exit {rc}, ' + ('OK' if ok else 'FAILED'))
        for ln in res_lines[:3]:
            print(f'      {ln[:220]}')
        results.append({'label': label, 'ok': ok, 'killed': killed, 'rc': rc, 'log': str(lp)})
        if not ok:
            rc_all = 1
            break
    case_dirs = [Path(f.value) for f in plan.door_flags + plan.caller_flags if f.bare == 'wwe2k14_case_dir']
    zips = [z for d in case_dirs for z in case_zips(d)]
    if plan.mode == 'game':
        if not case_dirs:
            print('[run] black box: none (this exe has no case hook, or --no-defaults)')
        elif zips:
            print(f'[run] black box: {len(zips)} case zip(s) in {case_dirs[0]}: ' + ', '.join(f'{z.name} ({z.stat().st_size // 1024} KB)' for z in zips[:5]))
        else:
            print(f'[run] black box: NO case zip in {case_dirs[0]} (the exe\'s RunExit / RunShot / RunFailure call sites may not be wired in yet)')
    (rd / 'result.json').write_text(json.dumps({'runs': results, 'case_zips': [str(z) for z in zips]}, indent=1), encoding='utf-8')
    print(f'[run] record: {rd}')
    return rc_all


# ----------------------------------------------------------------------------------------------------------- lint-all
def lint_all(out_path, repo=REPO, quiet=False, min_routes=100):
    root = Path(repo) / 'tools' / 'input_scripts'
    routes = sorted(p for p in root.rglob('*.txt') if not {'nav', 'android'} & set(p.relative_to(root).parts))
    skipped = sum(1 for p in root.rglob('*.txt') if {'nav', 'android'} & set(p.relative_to(root).parts))
    names = {n: k for n, k, _ in C.RULES}
    counts = {n: {'refuse': 0, 'warn': 0, 'as_speed': 0} for n in names}
    rows, unreadable = [], []
    for p in routes:
        try:
            rt = parse_route(p, repo)
        except Exception as e:  # noqa: BLE001
            unreadable.append((str(p), str(e)))
            continue
        guessed = 'speed' if p.name.startswith(('bench_', 'profile_', 'ab_')) else 'check'
        plan = C.Plan(kind=guessed, mode='game', exe=DEFAULT_EXE, route=rt, lint_only=True, main=MAIN, repo=Path(repo))
        fs = C.check_plan(plan)
        sp = C.Plan(kind='speed', mode='game', exe=DEFAULT_EXE, route=rt, lint_only=True, main=MAIN, repo=Path(repo))
        sfs = [f for f in C.rule_1_speed(sp) if f.severity == 'refuse']
        for rule in {f.rule for f in fs if f.severity == 'refuse'}:
            counts[rule]['refuse'] += 1
        for rule in {f.rule for f in fs if f.severity == 'warn'}:
            counts[rule]['warn'] += 1
        if sfs and guessed != 'speed':
            counts[1]['as_speed'] += 1
        rows.append((p.relative_to(root).as_posix(), guessed, fs, sfs if guessed != 'speed' else []))
    if len(rows) < min_routes:
        raise RuntimeError(f'only {len(rows)} routes read under {root}: the lint refuses to report on almost nothing')
    refused_routes = [r for r in rows if C.refused(r[2])]
    L = ['# Route lint (tools\\run\\run.py --lint-all)', '',
         f'Generated {datetime.datetime.now():%Y-%m-%d %H:%M}. {len(rows)} routes read under tools\\input_scripts (skipped: {skipped} in nav\\ [include snippets] and android\\ [phone]). '
         f'{len(unreadable)} unreadable. {len(refused_routes)} routes would be refused as they stand.', '',
         'Each route is judged with only its own header flags (no caller flags): a rule-2 finding "the caller must pass it" is a WARN here. '
         'Kind guess: bench_ / profile_ / ab_ routes as `--speed`, the rest as `--check`. The last column counts the routes that would be refused by rule 1 if run as `--speed`.', '',
         '| rule | key | routes refused | routes warned | refused if run as --speed |', '|---|---|---|---|---|']
    for n, k, _ in C.RULES:
        c = counts[n]
        L.append(f'| {n} | {k} | {c["refuse"]} | {c["warn"]} | {c["as_speed"] if n == 1 else "-"} |')
    L += ['', '## Routes that would be refused (and why)', '']
    def grouped(fs, sev):
        g = {}
        for f in fs:
            if f.severity == sev:
                g.setdefault((f.rule, f.key), []).append(f)
        return g
    for name, kind, fs, _ in refused_routes:
        for (rule, key), grp in grouped(fs, 'refuse').items():
            L.append(f'- `{name}` ({kind}) rule {rule} ({key}): {grp[0].msg}' + (f'  (+{len(grp) - 1} more of the same)' if len(grp) > 1 else ''))
    L += ['', '## Warnings', '']
    for name, kind, fs, _ in rows:
        for (rule, key), grp in grouped(fs, 'warn').items():
            L.append(f'- `{name}` rule {rule} ({key}): {grp[0].msg}' + (f'  (+{len(grp) - 1} more)' if len(grp) > 1 else ''))
    L += ['', '## Would be refused if run as --speed (rule 1)', '']
    for name, kind, fs, sfs in rows:
        for f in sfs:
            L.append(f'- `{name}`: {f.msg}')
    if unreadable:
        L += ['', '## Unreadable', ''] + [f'- {a}: {b}' for a, b in unreadable]
    outp = Path(out_path)
    outp.parent.mkdir(parents=True, exist_ok=True)
    outp.write_text('\n'.join(L) + '\n', encoding='utf-8')
    summary = {'routes': len(rows), 'refused_routes': len(refused_routes), 'counts': {names[n]: counts[n] for n in counts}, 'unreadable': len(unreadable), 'report': str(outp)}
    if not quiet:
        print(f'lint-all: {len(rows)} routes, {len(refused_routes)} would be refused, report {outp}')
        for n, k, _ in C.RULES:
            c = counts[n]
            print(f'  rule {n} {k:9} refuse {c["refuse"]:4}  warn {c["warn"]:4}' + (f'  as-speed {c["as_speed"]:4}' if n == 1 else ''))
    return summary


def main(argv=None):
    ap = make_parser()
    ns, rest = ap.parse_known_args(argv)
    if ns.lint_all:
        lint_all(ns.lint_out)
        return 0
    if not ns.route:
        ap.error('a ROUTE is required (or --lint-all)')
    try:
        plan = build_plan(ns, rest, dry=ns.dry_run)
    except FileNotFoundError as e:
        print(f'REFUSED: {e}')
        return 2
    findings = C.check_plan(plan)
    print_plan(plan, findings)
    if C.refused(findings):
        return 2
    if ns.dry_run:
        return 0
    return execute(plan, ns)


if __name__ == '__main__':
    sys.exit(main())
