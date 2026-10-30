"""Session 72 (Jarrod: "it more than doubled the time spent in the build process" -> "1 and 2"): when the cold-cache
entrance check (tools\\release\\entrance_check.ps1, ~3 min exclusive) may be left out of a release.

  py -I tools\\release\\entrance_check_reuse.py record --exe EXE --run N [--line "entrance check: PASS ..."]
      after a PASS: remember the game commit and SDK commit it tested (out\\release\\entrance_check_pass.json).
      Written only when the exe can stand for that commit: nothing that changes the game is uncommitted in either repo,
      and the exe was built after the commit (a dev build of older code is not recorded).
  py -I tools\\release\\entrance_check_reuse.py check
      exit 0 + "reuse: ..." when no file that can change the game exe changed since the recorded pass (item 2: a
      release with only docs / launcher / Android / script changes skips the check; item 1: a pass on a session's own
      build counts for the release of that code); exit 1 + "run: ..." otherwise.

What counts as "can change the game": everything except the paths in IRRELEVANT. The SDK counts by its commit: the
SDK commit of a release is the last STEPS line of tools\\sdk_patches\\export_series.py (make_release checks the export
equals wwe2k14-local), so patches\\ and the pins may change when that commit is still the recorded one. The check's
own scripts and route are relevant (a stricter check must run).
"""
import argparse
import datetime
import json
import os
import pathlib
import re
import subprocess
import sys

R = pathlib.Path(__file__).resolve().parents[2]
SDK = R / 'thirdparty' / 'rexglue-sdk-src'
STATE = R / 'out' / 'release' / 'entrance_check_pass.json'

# Paths that never go into wwe2k14.exe or change what it does in the check (prefixes, '/' separated).
IRRELEVANT = (
    'docs/', 'android/', 'assets/launcher/', '.claude/', '.gitignore', '.gitattributes', 'LICENSE', 'VERSION',
    'game_logo.', 'tools/', 'db/', 'patches/',
)
# ...except these (prefix match wins over IRRELEVANT).
RELEVANT = (
    'tools/release/entrance_check.ps1', 'tools/release/make_release.ps1', 'tools/perf/s70_entrance_report.py',
    'tools/input_scripts/short/s70_entrance_rt.txt', 'tools/build.ps1', 'tools/env.ps1', 'db/shaders/',
)


def git(repo, *args):
    r = subprocess.run(['git', '-C', str(repo), *args], capture_output=True, text=True)
    if r.returncode:
        sys.exit(f'git {" ".join(args)}: {r.stderr.strip()}')
    return r.stdout


def launcher_files_in_game():
    """src/launcher headers the game includes too (e.g. wm30_list.h from xctd_file.h): those count."""
    names = {p.name for p in (R / 'src' / 'launcher').glob('*')}
    used = set()
    for p in (R / 'src').rglob('*'):
        if p.is_file() and 'launcher' not in p.relative_to(R / 'src').parts and p.suffix in ('.h', '.cpp', '.glsl'):
            text = p.read_text(encoding='utf-8', errors='replace')
            used |= {n for n in names if n in text}
    return {f'src/launcher/{n}' for n in used}


def relevant(path, launcher_used):
    if path.endswith('.md') and not path.startswith('src/'):
        return False
    if path.startswith('src/launcher/'):
        return path in launcher_used
    if any(path.startswith(p) for p in RELEVANT):
        return True
    return not any(path.startswith(p) for p in IRRELEVANT)


CHECK_ONLY = ('tools/release/entrance_check.ps1', 'tools/perf/s70_entrance_report.py',
              'tools/input_scripts/short/s70_entrance_rt.txt')


def last_game_commit_time(launcher_used):
    """Commit time of the newest commit that changed a file the exe is built from (the check's own scripts don't
    count here: they change the check, not the exe). A docs commit after a release build keeps the build current."""
    log = git(R, 'log', '-n', '500', '--format=@%ct', '--name-only', 'HEAD')
    when = None
    for line in log.splitlines():
        if line.startswith('@'):
            when = int(line[1:])
        elif line and relevant(line, launcher_used) and line not in CHECK_ONLY:
            return when
    return 0


def sdk_commit_at(rev):
    text = git(R, 'show', f'{rev}:tools/sdk_patches/export_series.py')
    steps = re.findall(r"\('([^']+)',\s*'([0-9a-f]{40})'\)", text)
    return steps[-1][1] if steps else ''


def cmd_record(a):
    head = git(R, 'rev-parse', 'HEAD').strip()
    used = launcher_files_in_game()
    dirty = [l[3:] for l in git(R, 'status', '--porcelain').splitlines() if not l.startswith('??')]
    dirty = [p for p in dirty if relevant(p.replace('\\', '/'), used)]
    if dirty:
        print(f'entrance check record: not kept (uncommitted game changes: {", ".join(dirty[:5])})')
        return 0
    sdk_head = git(SDK, 'rev-parse', 'HEAD').strip()
    sdk_dirty = [l for l in git(SDK, 'status', '--porcelain', '--untracked-files=no').splitlines()
                 if not l[3:].startswith('thirdparty/')]  # submodule pointers (e.g. libmspack) are not built from here
    if sdk_dirty:
        print('entrance check record: not kept (the SDK has uncommitted changes)')
        return 0
    built = os.path.getmtime(a.exe)
    committed = last_game_commit_time(used)
    if built < committed:
        print('entrance check record: not kept (the exe is older than the last commit that changed the game)')
        return 0
    STATE.parent.mkdir(parents=True, exist_ok=True)
    rec = {'commit': head, 'sdk_commit': sdk_head, 'exe': str(a.exe), 'run': a.run, 'line': a.line,
           'when': datetime.datetime.now().strftime('%Y-%m-%d %H:%M')}
    STATE.write_text(json.dumps(rec, indent=2), encoding='utf-8')
    print(f'entrance check record: kept (game {head[:7]}, SDK {sdk_head[:7]}, run {a.run})')
    return 0


def cmd_check(_):
    if not STATE.exists():
        print('run: no earlier entrance check pass recorded')
        return 1
    rec = json.loads(STATE.read_text(encoding='utf-8'))
    head = git(R, 'rev-parse', 'HEAD').strip()
    if subprocess.run(['git', '-C', str(R), 'merge-base', '--is-ancestor', rec['commit'], head]).returncode:
        print(f'run: the recorded pass ({rec["commit"][:7]}) is not in this branch\'s history')
        return 1
    sdk_now = sdk_commit_at('HEAD')
    if sdk_now != rec['sdk_commit']:
        print(f'run: the SDK changed since the recorded pass ({rec["sdk_commit"][:7]} -> {sdk_now[:7]})')
        return 1
    used = launcher_files_in_game()
    changed = [p for p in git(R, 'diff', '--name-only', rec['commit'], head).splitlines() if p]
    hits = [p for p in changed if relevant(p, used)]
    if hits:
        more = f' (+{len(hits) - 5} more)' if len(hits) > 5 else ''
        print(f'run: game or check files changed since the recorded pass ({rec["commit"][:7]}): {", ".join(hits[:5])}{more}')
        return 1
    print(f'reuse: the pass of {rec["when"]} (run {rec["run"]}, game {rec["commit"][:7]}, SDK {rec["sdk_commit"][:7]}) '
          f'still stands: {len(changed)} file(s) changed since, none of them in the game')
    return 0


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('record')
    r.add_argument('--exe', required=True)
    r.add_argument('--run', default='')
    r.add_argument('--line', default='')
    sub.add_parser('check')
    a = ap.parse_args()
    return cmd_record(a) if a.cmd == 'record' else cmd_check(a)


if __name__ == '__main__':
    sys.exit(main())
