"""Session 101 (arc A4): the run contract's acceptance test. Offline; starts NO game (a tripwire on subprocess.Popen and on run.start_run proves it).
  py -I tools\\run\\selftest.py
Acceptance bar (docs\\agents\\s101\\lane7_run_contract.md):
  1. --dry-run on three real routes (a scene, a short route, a launcher route) prints a plan with every flag's source.
  2. Planted contradictions, each refused with its rule named, and the same routes without the fault pass.
  3. --lint-all finishes over every route; the report exists; the counts are printed.
  4. Nothing starts a game in the selftest.
"""
import contextlib
import io
import re
import subprocess
import sys
import tempfile
import textwrap
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import contract as C  # noqa: E402
import run as R  # noqa: E402

FAILS, PASSES = [], 0
STARTED = []   # anything the tripwire caught


def check(name, cond, detail=''):
    global PASSES
    if cond:
        PASSES += 1
        print(f'  PASS  {name}')
    else:
        FAILS.append(name)
        print(f'  FAIL  {name} {detail}')


# ---- tripwire: no game / run_list / run lock / launcher may be started from here (acceptance 4)
_real_popen = subprocess.Popen
_FORBID = re.compile(r'(run_list|runlock|wwe2k14\.exe|wwe2k14_launcher|run_and_triage)', re.I)


class _Trip(_real_popen):
    def __init__(self, args, *a, **k):
        text = ' '.join(args) if isinstance(args, (list, tuple)) else str(args)
        if _FORBID.search(text):
            STARTED.append(text)
            raise RuntimeError(f'TRIPWIRE: the selftest tried to start: {text[:160]}')
        super().__init__(args, *a, **k)


def _no_start(*a, **k):
    STARTED.append('run.start_run called')
    raise RuntimeError('TRIPWIRE: run.start_run called')


subprocess.Popen = _Trip
R.start_run = _no_start


def door(args):
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        try:
            rc = R.main(args)
        except SystemExit as e:
            rc = e.code
    return rc, buf.getvalue()


def refused_rules(out):
    return sorted({int(m) for m in re.findall(r'REFUSED rule (\d)', out)})


REPO = C.REPO
IS = REPO / 'tools' / 'input_scripts'
EXE = str(R.DEFAULT_EXE)
BASE = ['--dry-run', '--alive-games', '0', '--exe', EXE]
LAUNCH = ['--wwe2k14_launch=lesnar_bryan', '--wwe2k14_launch_skip_entrances=true', '--wwe2k14_cinecam=true']


def main():
    # ---------------- 1. three real routes
    print('1. --dry-run on three real routes')
    scene = None
    for p in sorted((IS / 'scenes').glob('*.txt')):
        rc, out = door([str(p)] + BASE)
        if rc == 0:
            scene = p
            break
    check('a clean scene route exists to plan', scene is not None, '(no scenes\\*.txt passes the contract as it stands)')
    plans = []
    if scene:
        plans.append(([str(scene)] + BASE, 'scene'))
    plans.append(([str(IS / 'short' / 'af_check.txt')] + LAUNCH + BASE, 'short route'))
    plans.append(([str(IS / 'short' / 'af_check.txt'), '--launcher', '--exit', '--no-save'] + BASE, 'launcher route'))
    for args, label in plans:
        rc, out = door(args)
        flags_lines = [ln for ln in out.splitlines() if re.match(r'^    \S.*\[.+\]\s*$', ln)]
        check(f'{label}: plan printed, verdict OK', rc == 0 and 'VERDICT: OK' in out and 'RUN PLAN' in out, f'rc={rc}\n{out[-600:]}')
        check(f'{label}: every flag line carries its source', len(flags_lines) >= 1 and all(re.search(r'\[[^\]]+\]\s*$', ln) for ln in flags_lines), str(flags_lines[:3]))
    rc, out = door([str(IS / 'short' / 'af_check.txt')] + LAUNCH + BASE)
    check('the command-line flags show "[command line]"', '--wwe2k14_launch=lesnar_bryan' in out and '[command line]' in out)
    sc = next(p for p in sorted((IS / 'scenes').glob('*.txt')) if 'harness: flags' in p.read_text(encoding='utf-8', errors='replace'))
    rc, out = door([str(sc)] + BASE)
    check('a scene\'s header flags show "[route header line N]"', '[route header line' in out, out[:600])

    # ---------------- 2. planted contradictions + controls
    print('2. planted contradictions (each refused with its rule), and the same routes without the fault')
    bench = str(IS / 'bench_lesnar_bryan.txt')
    af = str(IS / 'short' / 'af_check.txt')
    cases = [
        # (label, fault args, rule, control args)
        ('1 validation in a speed run', [bench, '--speed', '--wwe2k14_rdr_live_validation=1'], 1, [bench, '--speed']),
        ('1 fast-forward in a speed run', [bench, '--speed', '--vsync=false'], 1, [bench, '--speed']),
        ('1 a second game alive in a speed run', [bench, '--speed', '--alive-games', '1'], 1, [bench, '--speed']),
        ('1 a diagnostic switch in a speed run', [bench, '--speed', '--wwe2k14_file_trace=true'], 1, [bench, '--speed']),
        ('3 a misspelt flag', [af] + LAUNCH + ['--wwe2k14_cinecm=true'], 3, [af] + LAUNCH),
        ('3 a flag twice', [af] + LAUNCH + ['--wwe2k14_cinecam=false'], 3, [af] + LAUNCH),
        ('2 a launch route without its launch flag', [af, '--wwe2k14_launch_skip_entrances=true', '--wwe2k14_cinecam=true'], 2, [af] + LAUNCH),
        ('4 a launcher run without --no-save', [af, '--launcher', '--exit', '--no-defaults'], 4, [af, '--launcher', '--exit', '--no-save', '--no-defaults']),
        ('4 a launcher run without --exit', [af, '--launcher', '--no-save', '--no-defaults'], 4, [af, '--launcher', '--exit', '--no-save', '--no-defaults']),
        ('4 settings on, no settings file', [af] + LAUNCH + ['--wwe2k14_use_launcher_settings=true'], 4,
         [af] + LAUNCH + ['--wwe2k14_use_launcher_settings=true', f'--wwe2k14_settings_file={C.MAIN}\\out\\s101\\test_settings.json']),
        ('4 settings file outside out\\', [af] + LAUNCH + ['--wwe2k14_use_launcher_settings=true', '--wwe2k14_settings_file=C:\\Users\\x\\settings.json'], 4,
         [af] + LAUNCH + ['--wwe2k14_use_launcher_settings=true', f'--wwe2k14_settings_file={C.MAIN}\\out\\s101\\test_settings.json']),
        ('6 a picture A/B whose arms differ in more than the switch', [af] + LAUNCH + ['--picture', '--arm-a=--wwe2k14_cinecam_test_dof=0', '--arm-b=--wwe2k14_cinecam_test_dof=1 --wwe2k14_cinecam=false',
                                                                                   '--under-test', 'wwe2k14_cinecam_test_dof'], 6,
         [af] + LAUNCH + ['--picture', '--arm-a=--wwe2k14_cinecam_test_dof=0', '--arm-b=--wwe2k14_cinecam_test_dof=1', '--under-test', 'wwe2k14_cinecam_test_dof']),
        ('6 a picture A/B with identical arms', [af] + LAUNCH + ['--picture', '--arm-a=--wwe2k14_cinecam_test_dof=1', '--arm-b=--wwe2k14_cinecam_test_dof=1',
                                                                 '--under-test', 'wwe2k14_cinecam_test_dof'], 6,
         [af] + LAUNCH + ['--picture', '--arm-a=--wwe2k14_cinecam_test_dof=0', '--arm-b=--wwe2k14_cinecam_test_dof=1', '--under-test', 'wwe2k14_cinecam_test_dof']),
    ]
    for label, bad, rule, good in cases:
        rc, out = door(BASE + bad)
        check(f'refused: {label} (rule {rule} named)', rc == 2 and rule in refused_rules(out), f'rc={rc} rules={refused_rules(out)}\n{out[-500:]}')
        rc, out = door(BASE + good)
        check(f'passes without the fault: {label}', rc == 0, f'rc={rc} rules={refused_rules(out)}\n{out[-500:]}')

    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        bad_shot = td / 'entrance_bad.txt'
        bad_shot.write_text(textwrap.dedent('''\
            # planted: an entrance shot after a bare wait_frames
            on_fail quit
            wait_file 343.bik2 3600
            wait_frames 50
            screenshot late
            quit
            '''), encoding='utf-8')
        good_shot = td / 'entrance_good.txt'
        good_shot.write_text(textwrap.dedent('''\
            # control: the shot waits on game state
            on_fail quit
            wait_file 343.bik2 3600
            wait_state entrance_step_0 >=15 3600 2
            screenshot on_state
            quit
            '''), encoding='utf-8')
        rt_shot = td / 'entrance_realtime.txt'
        rt_shot.write_text('# harness: realtime (watched)\n' + bad_shot.read_text(encoding='utf-8'), encoding='utf-8')
        rc, out = door([str(bad_shot)] + BASE)
        check('refused: 5 a shot after a bare wait_frames in an entrance (rule 5 named)', rc == 2 and 5 in refused_rules(out), out[-400:])
        rc, out = door([str(good_shot)] + BASE)
        check('passes without the fault: 5 the shot waits on game state', rc == 0, out[-400:])
        rc, out = door([str(rt_shot)] + BASE)
        check('passes: 5 the same route is realtime (not fast-forwarded)', rc == 0, out[-400:])
        # a release-looking exe: needs the settings file and replay dir
        fake = td / 'rel' / 'wwe2k14.exe'
        fake.parent.mkdir()
        fake.write_bytes(b'MZ' + b'\0' * 64)
        args = ['--dry-run', '--alive-games', '0', '--exe', str(fake)]
        rc, out = door([af] + LAUNCH + args)
        check('refused: 4 a release exe without settings file / replay dir', rc == 2 and 4 in refused_rules(out), out[-400:])
        okflags = [f'--wwe2k14_settings_file={C.MAIN}\\out\\s101\\t.json', f'--wwe2k14_replay_dir={C.MAIN}\\out\\s101\\replays']
        rc, out = door([af] + LAUNCH + okflags + args)
        check('passes without the fault: 4 a release exe with both under out\\', rc == 0, out[-400:])
        # header-flag fault: a misspelt flag inside a route header is refused too
        hdr = td / 'hdr_bad.txt'
        hdr.write_text('# harness: flags --wwe2k14_fixd_seed=1\non_fail quit\nquit\n', encoding='utf-8')
        rc, out = door([str(hdr)] + BASE)
        check('refused: 3 a misspelt flag in a route header (source named)', rc == 2 and 3 in refused_rules(out) and 'route header line 1' in out, out[-400:])
        # the caller overriding a header flag is not a duplicate
        hdr2 = td / 'hdr_ok.txt'
        hdr2.write_text('# harness: flags --wwe2k14_fixed_seed=1\non_fail quit\nquit\n', encoding='utf-8')
        rc, out = door([str(hdr2), '--wwe2k14_fixed_seed=2'] + BASE)
        check('passes: a caller flag that overrides a header flag (the header\'s is left out)', rc == 0 and 'left out' in out, out[-400:])

        # ---------------- 3. lint-all
        print('3. --lint-all')
        report = C.MAIN / 'out' / 's101' / 'route_lint.md'
        if report.exists():
            report.unlink()
        s = R.lint_all(report, quiet=True)
        check('lint-all read every route (>= 500) with none unreadable', s['routes'] >= 500 and s['unreadable'] == 0, str(s))
        check('the report exists and has the counts table', report.is_file() and '| rule | key |' in report.read_text(encoding='utf-8'))
        for name, c in s['counts'].items():
            print(f'    rule {name:9} routes refused {c["refuse"]:4}  warned {c["warn"]:4}' + (f'  refused if run as --speed {c["as_speed"]:4}' if name == 'speed' else ''))
        print(f'    routes: {s["routes"]}, would be refused as they stand: {s["refused_routes"]}, report: {s["report"]}')
        # positive control: a tiny tree with planted faults must be counted
        tree = td / 'tree'
        (tree / 'tools' / 'input_scripts' / 'short').mkdir(parents=True)
        for i in range(3):
            (tree / 'tools' / 'input_scripts' / 'short' / f'ok{i}.txt').write_text('on_fail quit\nquit\n', encoding='utf-8')
        (tree / 'tools' / 'input_scripts' / 'short' / 'planted_flag.txt').write_text('# harness: flags --wwe2k14_fixd_seed=1\nquit\n', encoding='utf-8')
        (tree / 'tools' / 'input_scripts' / 'short' / 'planted_shot.txt').write_text('wait_file 343.bik2 10\nwait_frames 5\nscreenshot x\nquit\n', encoding='utf-8')
        (tree / 'tools' / 'input_scripts' / 'short' / 'planted_speed.txt').write_text('# harness: flags --wwe2k14_rdr_live_validation=1\nquit\n', encoding='utf-8')
        ps = R.lint_all(td / 'tree_lint.md', repo=tree, quiet=True, min_routes=1)
        check('lint positive control: the planted misspelt flag is counted (rule 3)', ps['counts']['flags']['refuse'] == 1, str(ps['counts']))
        check('lint positive control: the planted entrance shot is counted (rule 5)', ps['counts']['shots']['refuse'] == 1, str(ps['counts']))
        check('lint positive control: the planted validation flag counts as speed-refused', ps['counts']['speed']['as_speed'] == 1, str(ps['counts']))
        check('lint positive control: clean routes are not counted', ps['refused_routes'] == 2, str(ps))

    # the start commands parse as PowerShell (parsed through [scriptblock]::Create from an env var: nothing is executed)
    import os
    rt = R.parse_route(IS / 'short' / 'af_check.txt', REPO) if hasattr(R, 'parse_route') else None
    plan = C.Plan(kind='speed', mode='game', exe=Path(EXE), route=rt)
    cmds = {'game speed': R.game_command(plan, rt.path, [C.Flag('--wwe2k14_fixed_seed', '1', 't')], 700)[-1],
            'game check': R.game_command(C.Plan(kind='check', mode='game', exe=Path(EXE), route=rt), rt.path, [], 700)[-1]}
    check('a speed start passes -Exclusive, a check start does not', ' -Exclusive' in cmds['game speed'] and ' -Exclusive' not in cmds['game check'])
    for label, text in cmds.items():
        env = dict(os.environ, CMDTEXT=text)
        r = subprocess.run(['pwsh', '-NoProfile', '-Command', '$e=$null; $t=$null; [void][System.Management.Automation.Language.Parser]::ParseInput($env:CMDTEXT,[ref]$t,[ref]$e); if ($e.Count) { $e | % { $_.Message }; exit 1 }'],
                           capture_output=True, text=True, env=env, timeout=60)
        check(f'the {label} start command parses as PowerShell', r.returncode == 0, r.stdout + r.stderr)

    # ---------------- 4. nothing started
    print('4. nothing started a game')
    check('no process matching run_list / runlock / wwe2k14 was started', not STARTED, str(STARTED))
    src = (Path(R.__file__)).read_text(encoding='utf-8')
    check('run.py starts a process in exactly one place (start_run)', len(re.findall(r'subprocess\.Popen\(', src)) == 1)
    # the tripwire itself works (a positive control)
    try:
        subprocess.Popen(['pwsh', '-File', 'tools/bench/run_list.ps1'])
        tripped = False
    except RuntimeError:
        tripped = True
    STARTED.clear()
    check('the tripwire fires when run_list is started (positive control)', tripped)
    try:
        R.start_run(['x'], Path('x'), False, 1)
        tripped2 = False
    except RuntimeError:
        tripped2 = True
    STARTED.clear()
    check('run.start_run is replaced by the tripwire in the selftest', tripped2)

    print(f'\nrun contract selftest: {PASSES} passed, {len(FAILS)} failed')
    for f in FAILS:
        print(f'  FAILED: {f}')
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
