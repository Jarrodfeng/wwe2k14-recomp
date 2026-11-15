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
# S102 lane M4 (the fast gate; library tools\gate\fastgate.py, selftest tools\gate\fastgate_selftest.py):
#   --changed [BASE]   run only the checks a change can affect: a lane json (or one check) declares "affects": [globs]; a check is selected when a path in
#                      `git diff --name-only --no-renames BASE` (plus untracked files) matches, or its lane json changed. No "affects" = always runs.
#                      BASE defaults to the merge-base of HEAD and s102-arc-pt2 (the whole branch's change); pass one when that branch is not here.
#                      Implies --jobs 4, the result cache, and the stale-build refusal below: THE FAST GATE NEVER REBUILDS, so every check that runs
#                      fview / rplay with --no-build (inferred, tools\gate\exes.py) FAILs "stale build" until the exe is built (`py -I tools\gate\exes.py
#                      ensure out/renderer/dev/fview.exe`); --build-stale restores the old build-once behaviour.
#   affects            must be a non-empty list of globs and every glob must match a file, else the gate FAILs at load (a lane that never, or always,
#                      runs by a typo is worse than a loud one).
#   --jobs N           lanes in parallel (N workers); the checks of one lane stay in order. Default 1 (4 with --changed). A lane with a check that has no
#                      "affects" or that runs a .ps1 build runs on ONE serial worker (lane after lane); "parallel_safe": true on a check opts out.
#   cache              a PASS is cached by (lane json, check cmd, content of every file its affects globs match, every `needs` path (file by content, folder
#                      by file sizes and times), every cmd argument that is an existing file, the Python version, an env allow-list, the built programs);
#                      a check without "affects" or with "cache": false is never cached. --no-cache turns it off. out\gate\cache.json, replaced atomically.
#   "programs": [{"path": "out/x/tool.exe", "sources": [globs]}]  on a check: a missing or older-than-its-sources program is FAIL "stale build: ...", never a pass.
#   out\gate\times.csv one row per check and one "(whole gate)" row per run (seconds, cache hit or not).
import argparse, json, os, re, subprocess, sys, threading, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import exes as gexes
import fastgate as fg   # S102 M4: --changed / cache / stale programs / times.csv
from concurrent.futures import ThreadPoolExecutor

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
    ap.add_argument('--changed', nargs='?', const='', default=None, metavar='BASE', help='S102 M4: only the checks the change since BASE can affect (default BASE: merge-base with s102-arc-pt2)')
    ap.add_argument('--jobs', type=int, default=0, help='S102 M4: lanes in parallel (default 1; 4 with --changed)')
    ap.add_argument('--no-cache', action='store_true', help='S102 M4: ignore and do not write the result cache')
    ap.add_argument('--build-stale', action='store_true', help='S102 M4: with --changed, build a stale tool exe (the old behaviour) instead of FAIL stale build')
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
            c['_lane_rel'] = f'tools/gate/lanes/{f.name}'   # S102 M4
            c['_lane_bytes'] = f.read_bytes()
            if 'affects' not in c and 'affects' in data:
                c['affects'] = data['affects']
            checks.append(c)
    only = {x for x in args.only.split(',') if x}
    if only:
        checks = [c for c in checks if c['name'] in only or c['lane'] in only]
    t_gate = time.time()   # S102 M4 from here
    fast = args.changed is not None
    jobs = args.jobs or (4 if fast else 1)
    use_cache = fast and not args.no_cache
    refuse_stale = fast and not args.build_stale
    all_files = None
    if any(fg.lane_affects(c) is not None for c in checks):
        all_files = fg.tree_files(root)
        bad_aff = fg.validate_affects(root, checks, all_files)
        if bad_aff:
            for b in bad_aff:
                print(f'FAIL  lane affects: {b}')
            return 1
    if fast:
        try:
            base = args.changed or fg.default_base(root)
            changed = fg.changed_files(root, base)
        except Exception as e:
            print(f'FAIL  --changed {args.changed or "(merge-base)"}: {e}')
            return 1
        before = len(checks)
        checks = [c for c in checks if fg.selected(c, changed, c['_lane_rel'])[0]]
        print(f"[gate] --changed {base}: {len(changed)} changed path(s); {len(checks)} of {before} checks selected "
              f"({len({c['lane'] for c in checks})} lanes)", flush=True)
        if all_files is None and checks:
            all_files = fg.tree_files(root)
        all_files = all_files if all_files is not None else []
    if args.list:
        for c in checks:
            print(f"{c['lane']:12} {c['name']:28} {' '.join(c['cmd'])}")
        return 0
    if not checks and not fast:
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
            if sp['path'] not in exe_state and refuse_stale:   # S102 M4: the fast gate never rebuilds; a stale exe is a FAIL
                fresh, why = gexes.is_fresh(root, sp)
                exe_state[sp['path']] = ('fresh', f"{sp['path']} is fresh") if fresh else (
                    'STALE', f"stale build: {sp['path']} {why} (build it, or --build-stale)")
                print(f"[gate] {exe_state[sp['path']][1]}", flush=True)
            elif sp['path'] not in exe_state:
                exe_state[sp['path']] = gexes.ensure(root, sp)
                print(f"[gate] {exe_state[sp['path']][1]}" + ('  <- BUILD FAILED' if exe_state[sp['path']][0] == 'BUILD' else ''), flush=True)
            c['_exes'].append(sp['path'])
    cache_path = (Path(args.out) if args.out else MAIN / 'out' / 'gate') / 'cache.json'
    cache = fg.load_cache(cache_path) if use_cache else {}
    cache_lock = threading.Lock()
    print_lock = threading.Lock()

    def run_check(c):
        missing = [n for n in c.get('needs', []) if not (MAIN / n).exists()]
        t0 = time.time()
        bad = [exe_state[e][1] for e in c['_exes'] if exe_state[e][0] == 'BUILD']
        stale = [exe_state[e][1] for e in c['_exes'] if exe_state[e][0] == 'STALE']
        progs = fg.program_list(c)   # S102 M4: programs the check declares must be as new as their sources, in every mode
        for pr in progs:
            why = fg.stale_program(root, pr, all_files)
            if why:
                stale.append(f'stale build: {why}')
        key, hit = None, False
        if use_cache and not missing and not bad and not stale:
            key = fg.cache_key(root, c, c['_lane_bytes'], [e for e in c['_exes'] if exe_state[e][0] != 'STALE'] + [pr['path'] for pr in progs], all_files, MAIN)
            hit = key is not None and cache.get(key, {}).get('verdict') == 'PASS'
        if missing:
            verdict, tail = 'SKIP', 'missing: ' + ', '.join(missing)
        elif bad:
            verdict, tail = 'BUILD', bad[0] + ' (check not run)'
        elif stale:
            verdict, tail = 'FAIL', stale[0] + ' (check not run)'
        elif hit:
            verdict, tail = 'PASS', 'cache hit (same lane json, inputs and programs as the passing run of ' + cache[key].get('when', '?') + ')'
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
        if key is not None and verdict == 'PASS' and not hit:
            with cache_lock:
                cache[key] = {'verdict': 'PASS', 'when': time.strftime('%Y-%m-%d %H:%M'), 'check': c['name']}
        res = {'lane': c['lane'], 'name': c['name'], 'verdict': verdict, 'seconds': round(dt, 1), 'last': tail, 'exes': c['_exes'], 'cache': 'hit' if hit else ''}
        with print_lock:
            print(f"{verdict:5} {c['lane']:12} {c['name']:28} {dt:6.1f} s  {tail[:150]}", flush=True)
        return res

    def run_lane(lane_checks):
        return [run_check(c) for c in lane_checks]   # the checks of one lane keep their order (build, then unit, then proof)

    by_lane = {}
    for c in checks:
        by_lane.setdefault(c['lane'], []).append(c)
    if jobs <= 1:
        lane_results = [run_lane(v) for v in by_lane.values()]
    else:
        # S102 M4 review: a lane with a check that has no "affects" (nothing says what it writes) or that runs a .ps1 build runs on ONE serial
        # worker, lane after lane; the other lanes share the pool. "parallel_safe": true on a check opts out.
        lanes = list(by_lane.values())
        serial = [v for v in lanes if any(fg.serial_needed(c) for c in v)]

        def run_serial():
            return [run_lane(v) for v in serial]
        with ThreadPoolExecutor(max_workers=max(1, jobs)) as ex:
            fut_serial = ex.submit(run_serial) if serial else None
            futs = {id(v): ex.submit(run_lane, v) for v in lanes if not any(v is s for s in serial)}
            ser_res = dict(zip((id(v) for v in serial), fut_serial.result())) if fut_serial else {}
            lane_results = [ser_res[id(v)] if id(v) in ser_res else futs[id(v)].result() for v in lanes]
    results = [r for lr in lane_results for r in lr]
    failed = sum(r['verdict'] in ('FAIL', 'BUILD') for r in results)
    out_dir = Path(args.out) if args.out else MAIN / 'out' / 'gate'
    out_dir.mkdir(parents=True, exist_ok=True)
    wall = time.time() - t_gate
    if use_cache:
        fg.save_cache(cache_path, cache)
    mode = f'changed:{args.changed}' if fast else 'full'
    now = time.strftime('%Y-%m-%d %H:%M:%S')
    fg.append_times(out_dir / 'times.csv', [[now, str(root), mode, r['lane'], r['name'], r['verdict'], r['seconds'], r['cache']] for r in results]
                    + [[now, str(root), mode, '(whole gate)', f'{len(results)} checks, {jobs} jobs', 'FAIL' if failed else 'PASS', round(wall, 1), '']])
    (out_dir / 'last.json').write_text(json.dumps({'when': time.strftime('%Y-%m-%d %H:%M'), 'root': str(root), 'results': results}, indent=1), encoding='utf-8')
    nbuild = sum(r['verdict'] == 'BUILD' for r in results)
    print(f"gate: {len(results) - failed} of {len(results)} pass or skip, {failed} fail"
          + (f" ({nbuild} of them BUILD: a tool exe did not build, not a verdict on the code)" if nbuild else '') + f", {wall:.1f} s wall")
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
