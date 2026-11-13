# The gate (Session 101, RENDERER_ARC.md section 5A rule 3): runs every offline check that needs no game, one per lane, and prints
# one table. Integration is reading that table.
#   py -I tools\gate\gate.py [--only NAME[,NAME]] [--list] [--root DIR] [--out DIR] [--no-exes]
# Each lane owns one file, tools\gate\lanes\<lane>.json (so lanes never edit the same file):
#   {"checks": [{"name": "fview.cards", "cmd": ["py", "-I", "tools/fview/selftest.py"], "timeout_s": 300, "needs": ["out/captures/s100cards"]}]}
# cmd runs from the repo root (or --root: a worktree). "needs" lists paths (relative to the MAIN checkout, where out\ and game_data\ live)
# that must exist, else the check is SKIP (not PASS). Exit code: 0 all PASS / SKIP, 1 any FAIL. Results: out\gate\last.json.
# A check must fail loudly when it cannot read its input (LESSONS: a checker that silently reads nothing).
# Tool exes (S101 lane 163): a check that runs fview.exe / rplay.exe from the checkout must not run on a stale or half-linked one. Declare them in the
# lane ("exes": ["out/renderer/dev/fview.exe"]); a --no-build check using fview / rplay is inferred (tools\gate\exes.py reads its script). Before the
# first check the gate decides stale vs fresh (the exe older than a source it is built from), builds a stale one ONCE under a lock (<exe>.lock, pid;
# a second gate run waits and then finds it fresh), and prints `[gate] built fresh <exe> (...)` or `[gate] <exe> is fresh`. A failed build or link
# (also one printed by a check's own nested build) shows as BUILD with the exe and the cause, never as a check FAIL; an exe held by a running process
# is waited for (GATE_EXE_WAIT_S, default 600 s) and then named (pid, command line); the other process is never killed. Selftest: tools\gate\selftest.py.
import argparse, json, os, re, subprocess, sys, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import exes as gexes

def _main_checkout():
    # The main checkout (where out\ and game_data\ live), also when this file runs from a worktree: git's common dir is the main .git.
    here = Path(__file__).resolve().parents[2]
    try:
        common = subprocess.run(['git', '-C', str(here), 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True,
                                text=True, timeout=20).stdout.strip()
        if common:
            return Path(common).parent
    except Exception:
        pass
    return here


MAIN = _main_checkout()


def _resolve_cmd(cmd, root):
    # Windows starts a relative exe from the parent's folder, not cwd: make a relative path that exists under root absolute.
    first = Path(cmd[0])
    if not first.is_absolute() and (root / first).exists():
        cmd = [str(root / first)] + cmd[1:]
    # Lane 300: an input path given as out/... (captures live in the MAIN checkout's out\, "needs" is read there) must also resolve when the gate runs
    # from a worktree root: an argument that exists under MAIN but not under root (a trailing :N / :name suffix is allowed) becomes the MAIN path.
    if root != MAIN:
        fixed = []
        for a in cmd:
            if a.startswith('out/') and not (root / a.split(':')[0]).exists() and (MAIN / a.split(':')[0]).exists():
                a = str(MAIN / a)
            fixed.append(a)
        cmd = fixed
    return cmd


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--only', default='')
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--root', default=str(MAIN))
    ap.add_argument('--out', default='', help='results folder (default: <main checkout>/out/gate; the selftest uses a temp one)')
    ap.add_argument('--no-exes', action='store_true', help='skip the exe freshness step (the pre-S101-lane-163 behaviour, for A/B)')
    args = ap.parse_args()
    root = Path(args.root).resolve()
    lanes_dir = root / 'tools' / 'gate' / 'lanes'
    checks = []
    for f in sorted(lanes_dir.glob('*.json')):
        try:
            data = json.loads(f.read_text(encoding='utf-8'))
        except Exception as e:
            print(f'FAIL  {f.name}: unreadable lane file ({e})')
            return 1
        for c in data.get('checks', []):
            c['lane'] = f.stem
            checks.append(c)
    only = {x for x in args.only.split(',') if x}
    if only:
        checks = [c for c in checks if c['name'] in only or c['lane'] in only]
    if args.list:
        for c in checks:
            print(f"{c['lane']:12} {c['name']:28} {' '.join(c['cmd'])}")
        return 0
    if not checks:
        print('FAIL  no checks found (tools\\gate\\lanes\\*.json)')
        return 1
    results, failed = [], 0
    # Lane 300: the unpacked captures under out\s101\lane92 are a cache of the kept case zips (a disk clean-up removes them): rebuild what is missing
    # before any needs are read (tools\gate\inputs.py is the table; a missing zip is named by the check's SKIP / FAIL below).
    if os.environ.get('GATE_NO_RESTORE') != '1':
        try:
            import inputs as ginputs
            ginputs.restore_all()
        except Exception as e:
            print(f'[gate] inputs restore skipped: {e}')
    reg = gexes.registry()
    exe_state = {}   # normalised path -> (state, text), once per gate run
    for c in checks:
        c['_exes'] = []
        if [n for n in c.get('needs', []) if not (MAIN / n).exists()]:
            continue
        for e in ([] if args.no_exes else gexes.infer(c, root, reg)):
            sp = gexes.spec_for(e, reg)
            if sp is None:
                key = gexes.norm(e) if isinstance(e, str) else str(e)
                exe_state.setdefault(key, ('BUILD', f'{key}: no build recipe known (give an inline spec in the lane)'))
                c['_exes'].append(key)
                continue
            if sp['path'] not in exe_state:
                exe_state[sp['path']] = gexes.ensure(root, sp)
                print(f"[gate] {exe_state[sp['path']][1]}" + ('  <- BUILD FAILED' if exe_state[sp['path']][0] == 'BUILD' else ''), flush=True)
            c['_exes'].append(sp['path'])
    for c in checks:
        missing = [n for n in c.get('needs', []) if not (MAIN / n).exists()]
        t0 = time.time()
        bad = [exe_state[e][1] for e in c['_exes'] if exe_state[e][0] == 'BUILD']
        if missing:
            verdict, tail = 'SKIP', 'missing: ' + ', '.join(missing)
        elif bad:
            verdict, tail = 'BUILD', bad[0] + ' (check not run)'
        else:
            env = dict(os.environ, RDR_MAIN_CHECKOUT=str(MAIN))
            try:
                p = subprocess.run(_resolve_cmd(c['cmd'], root), cwd=root, capture_output=True, text=True, timeout=c.get('timeout_s', 300), env=env)
                verdict = 'PASS' if p.returncode == 0 else 'FAIL'
                out = (p.stdout + p.stderr).strip().splitlines()
                tail = out[-1] if out else '(no output)'
                if verdict == 'FAIL':
                    tail = ' | '.join(out[-4:])
                # a nested build or link error in the check's own output is a BUILD, not a verdict on the code (a PASS exit with a "FAIL ... build"
                # line means the check ran on an exe it could not rebuild: the same)
                joined = '\n'.join(out)
                cause = gexes.build_cause(joined)
                if cause and (verdict == 'FAIL' or re.search(r'^\s*FAIL\b.*\b(fview|renderer|rplay) build\b', joined, re.M | re.I)):
                    verdict, tail = 'BUILD', cause
            except subprocess.TimeoutExpired:
                verdict, tail = 'FAIL', f"timeout after {c.get('timeout_s', 300)} s"
            except FileNotFoundError as e:
                verdict, tail = 'FAIL', f'cannot start: {e}'
        dt = time.time() - t0
        failed += verdict in ('FAIL', 'BUILD')
        results.append({'lane': c['lane'], 'name': c['name'], 'verdict': verdict, 'seconds': round(dt, 1), 'last': tail, 'exes': c['_exes']})
        print(f"{verdict:5} {c['lane']:12} {c['name']:28} {dt:6.1f} s  {tail[:150]}")
    out_dir = Path(args.out) if args.out else MAIN / 'out' / 'gate'
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / 'last.json').write_text(json.dumps({'when': time.strftime('%Y-%m-%d %H:%M'), 'root': str(root), 'results': results}, indent=1), encoding='utf-8')
    nbuild = sum(r['verdict'] == 'BUILD' for r in results)
    print(f"gate: {len(results) - failed} of {len(results)} pass or skip, {failed} fail"
          + (f" ({nbuild} of them BUILD: a tool exe did not build, not a verdict on the code)" if nbuild else ''))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
