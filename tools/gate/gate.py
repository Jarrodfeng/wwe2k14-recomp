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
#                      BASE defaults to the merge-base of HEAD and main (the whole branch's change); pass one when that branch is not here.
#                      Implies --jobs 4, the result cache, and the stale-build refusal below: THE FAST GATE NEVER REBUILDS, so every check that runs
#                      fview / rplay with --no-build (inferred, tools\gate\exes.py) FAILs "stale build" until the exe is built (`py -I tools\gate\exes.py
#                      ensure out/renderer/dev/fview.exe`); --build-stale restores the old build-once behaviour.
#   affects            must be a non-empty list of globs and every glob must match a file, else the gate FAILs at load (a lane that never, or always,
#                      runs by a typo is worse than a loud one).
#   --jobs N           lanes in parallel (N workers); the checks of one lane stay in order. Default (S103 PS2): a quarter of the logical CPUs, 2..8 (GATE_JOBS).
#                      There is no serial worker any more: two checks run side by side unless one WRITES a path the other reads or writes (tools\gate\sched.py:
#                      footprints from the lane's "writes": [...] / "reads": [...], else the command line and the scripts it runs; a check whose writes cannot be
#                      derived is a barrier that runs alone and is listed by --footprints). "parallel_safe": true still opts a check out of every lock.
#   --gpu-slots N      checks that use the GPU (rig renders, fview, rplay: inferred from the script's code, or "gpu": true / false) share N slots, default 10
#                      (GATE_GPU_SLOTS), as OS-locked files in <main checkout>\out\gate\gpu_slots: every gate on this PC shares them, a killed process frees its slot.
#   GATE_CHECK_DIR     every check runs with its own scratch folder <checkout>\out\gate\chk\<check name> (emptied first); run_rplay.py puts its picture there.
#   cache              a PASS is cached by (lane json, check cmd, content of every file its affects globs match, the CONTENT of every `needs` path and every
#                      out\ / game_data\ path the command names or its scripts name in their code (a capture folder: every file's FULL bytes; MAIN's out\build
#                      build files; the tool exes a script runs), the check's script and its import closure, this gate's own code, the Python version, an env
#                      allow-list, the toolchain, the built programs / exes by bytes). The key is taken UNDER the check's path locks (a producer check has
#                      finished or not started): a hit is never a stale PASS. A .ps1 run from a -Command body or one that runs other scripts is uncacheable
#                      unless the lane declares "exes" / "reads";
#                      a check without "affects", with "cache": false, an import we cannot find, or a planted-fault twin without its paired real check is never
#                      cached and its times.csv cache column says why ("uncacheable: ..."). A twin (--plant / --expect-fail) is cached only together with the
#                      real check of its lane, under a key bound to the real check's. --no-cache turns it off. out\gate\cache.json, replaced atomically.
#   --verify-cache / --verify-p P / GATE_VERIFY_P   re-run a cache hit for real (every hit, or with probability P, default 0.05) and FAIL "CACHE MISMATCH" when the
#                      verdict is not the cached PASS (the entry is dropped): the proof that a key is complete, paid for in a few percent of the hits.
#   --footprints       print each selected check's reads / writes / GPU / cacheability and why; run nothing.
#   "programs": [{"path": "out/x/tool.exe", "sources": [globs]}]  on a check: a missing or older-than-its-sources program is FAIL "stale build: ...", never a pass.
#   out\gate\times.csv one row per check and one "(whole gate)" row per run (seconds, cache hit or not, `run` = the gate's RUN_ID); appended under times.csv.lock, cache.json under cache.json.lock.
# S103 GST fix round: --tier cheap|all (tools\gate\tiers.py: the per-piece gate is the CHEAP checks; a check named lane.check in --only always runs; gate line ends `rig skipped: N`); exit 5 = the only
#   non-pass rows are NOT RUN (--deadline), gate_wait.py maps it to 2; GATE_SLOTS_DIR moves the GPU slot folder (selftests); GATE_MIN_LEFT_S (default 10) = no check starts with less left before --deadline.
import argparse, json, os, re, shutil, subprocess, sys, threading, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import exes as gexes
import fastgate as fg   # S102 M4: --changed / cache / stale programs / times.csv
import toolcache as gtc   # S103 GX: content-addressed tool-exe cache shared by all checkouts (copy, never link)
import sched as gsched   # S103 PS2: path-conflict scheduling (footprints), GPU slots, default jobs
import proofkey as gpk   # S103 PS2: the honest proof cache's input enumeration
import deps as gdeps   # S103 DIB: dependency manifests
import gs_share as gsh   # S103 DIB: one shared game_syntax run for all lanes
import tiers as gtiers   # S103 GST: cheap | rig check tiers (the per-piece gate is the CHEAP checks)
import needs_main as gnm   # S103 WTG: a row that needs what only the MAIN checkout has is NEEDS-MAIN at once (no run, no timeout)
import random
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
RUN_ID = time.strftime('%Y%m%d-%H%M%S') + '-%d' % os.getpid()   # S103 GSP2: names this gate run's fail logs (two gates in one tree never overwrite each other's)


def run_tree(cmd, cwd, timeout, env):
    """subprocess.run with a timeout that kills the WHOLE process tree (S103 PS2 review: killing only the py child left a grandchild fview.exe holding the GPU)."""
    proc = subprocess.Popen(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
    try:
        out, err = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        if os.name == 'nt':
            subprocess.run(['taskkill', '/F', '/T', '/PID', str(proc.pid)], capture_output=True)
        else:
            proc.kill()
        try:
            proc.communicate(timeout=20)
        except Exception:
            pass
        raise
    return subprocess.CompletedProcess(cmd, proc.returncode, out, err)


def head_of(root):
    """The checkout's HEAD sha (None when git cannot say)."""
    try:
        p = subprocess.run(['git', '-C', str(root), 'rev-parse', 'HEAD'], capture_output=True, text=True, timeout=20)
        return p.stdout.strip() if p.returncode == 0 else None
    except Exception:
        return None


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
            # S103: game_data/ too (the player's files live only in the MAIN checkout; lanes used to write ../../../game_data, which only resolves from a worktree)
            if a.startswith(('out/', 'game_data/')) and not (root / a.split(':')[0]).exists() and (MAIN / a.split(':')[0]).exists():
                a = str(MAIN / a)
            fixed.append(a)
        cmd = fixed
    return cmd


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--only', action='append', default=[], help='lanes or lane.check, comma-separated; may repeat (S103: repeated --only used to keep only the last)')
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--root', default=str(Path(__file__).resolve().parents[2]), help='DPC: the tree to gate; default = the checkout this gate.py lives in, so a worktree gate compares its OWN HEAD (it used to default to the main checkout, whose HEAD moves with every main commit)')
    ap.add_argument('--out', default='', help='results folder (default: <main checkout>/out/gate; the selftest uses a temp one)')
    ap.add_argument('--no-exes', action='store_true', help='skip the exe freshness step (the pre-S101-lane-163 behaviour, for A/B)')
    ap.add_argument('--changed', nargs='?', const='', default=None, metavar='BASE', help='S102 M4: only the checks the change since BASE can affect (default BASE: merge-base with main)')
    ap.add_argument('--jobs', type=int, default=0, help='S103 PS2: lanes in parallel (default: a quarter of the logical CPUs, 2..8; GATE_JOBS overrides). Two checks run side by side unless one writes a path the other reads or writes (see --footprints)')
    ap.add_argument('--deadline', type=float, default=0.0, metavar='S', help='S103 GST: after S seconds (from the gate start) nothing new is dequeued: the rest is NOT RUN (a FAIL for the exit code), a running check is timed out at the deadline, and times.csv / cache.json / the summary line are still written (an outer `timeout` then never kills the gate blind; gate_wait.py passes its own cap minus a margin)')
    ap.add_argument('--tier', choices=('cheap', 'all'), default='all', help='S103 GST: cheap = builds, compiles, lints, units / selftests and the checks NAMED in --only (a lane named in --only runs its cheap checks); rig-tier checks (rig / picture / GPU proofs) are skipped and counted (`rig skipped: N`). all (default) = a milestone / gate_main. gate_wait.py passes cheap for --only')
    ap.add_argument('--gpu-slots', type=int, default=-1, help='S103 PS2: checks that use the GPU (rig renders, fview, rplay) share N slots across every gate on this PC (default 10; GATE_GPU_SLOTS; 0 = no limit)')
    ap.add_argument('--verify-cache', action='store_true', help='S103 PS2: re-run EVERY cache hit for real and FAIL loudly when its verdict differs from the cached PASS')
    ap.add_argument('--verify-p', type=float, default=-1.0, help='S103 PS2: the probability a cache hit is re-run and compared (default 0.05; GATE_VERIFY_P)')
    ap.add_argument('--footprints', action='store_true', help='S103 PS2: print what each selected check reads / writes (and why), whether it takes a GPU slot, and whether it is cacheable; run nothing')
    ap.add_argument('--why', default='', metavar='CHECK', help='S103 DIB: explain why CHECK (or every check of a lane) is selected or skipped by --changed (derived deps, affects, the changed path that matched); run nothing')
    ap.add_argument('--base', default='', metavar='REV', help='S103 OMIT: the revision --only diffs against to name the lanes it leaves out (default: merge-base with s103-leave-the-stream on a worktree branch, else with main)')
    ap.add_argument('--strict-only', action='store_true', help='S103 OMIT: exit 1 before running when --only leaves out a lane whose affects match the change (default: warn, exit code unchanged)')
    ap.add_argument('--no-cache', action='store_true', help='ignore and do not write the result cache (builders never pass it)')
    ap.add_argument('--build-stale', action='store_true', help='S102 M4: with --changed, build a stale tool exe (the old behaviour) instead of FAIL stale build')
    ap.add_argument('--assume-main', action='store_true', help='S103 WTG: this tree IS where the rows are meant to run (gate_main.py): NEEDS-MAIN is impossible, a missing file is a plain FAIL')
    ap.add_argument('--no-toolcache', action='store_true', help='S103 GX: do not use or fill the tool-exe cache (out\\toolcache of the main checkout)')
    ap.add_argument('--toolcache-dir', default='', help='S103 GX: the tool-exe cache folder (default: <main checkout>/out/toolcache)')
    args = ap.parse_args()
    root = Path(args.root).resolve()
    deadline_at = (time.time() + args.deadline) if args.deadline > 0 else None   # S103 GST
    head0 = head_of(root)   # S103 DIB2: the tree must not move under a running gate (a merge mid-gate ran a script against an exe built from older code)
    moved = {'to': None, 'n': 0}
    tcache = None if args.no_toolcache else gtc.ToolCache(root, args.toolcache_dir or (MAIN / 'out' / 'toolcache'),
                                                           log=lambda m: print(m, flush=True), main=MAIN)
    lanes_dir = root / 'tools' / 'gate' / 'lanes'
    checks = []
    for f in sorted(lanes_dir.glob('*.json')):
        try:
            data = json.loads(f.read_text(encoding='utf-8'))
        except Exception as e:
            print(f'FAIL  {f.name}: unreadable lane file ({e})')
            return 1
        lane_all = data.get('checks', [])   # S103 PS2: the whole lane, before --only / --changed (a planted-fault twin pairs with a real check of its lane)
        for c in lane_all:
            c['_lane_all'] = lane_all
            c['lane'] = f.stem
            c['_lane_rel'] = f'tools/gate/lanes/{f.name}'   # S102 M4
            c['_lane_bytes'] = f.read_bytes()
            if 'affects' not in c and 'affects' in data:
                c['affects'] = data['affects']
            if 'tier' not in c and 'tier' in data:
                c['tier'] = data['tier']
            checks.append(c)
    only = {x for x in ','.join(args.only).split(',') if x}
    all_lane_checks = list(checks)   # S103 OMIT: before --only filters
    if only:
        checks = [c for c in checks if c['name'] in only or c['lane'] in only]
    omitted = None   # S103 OMIT: {lane: [paths]} the --only list leaves out although the change touches their affects (None = no --only)
    omit_err = False
    if only:
        try:
            ob = args.base or args.changed or fg.only_base(root)
            omitted = fg.omitted_affected(all_lane_checks, only, fg.changed_files(root, ob))
        except Exception as e:
            omit_err = True
            print(f'[gate] --only omission check failed: {e}', flush=True)
            if args.strict_only:
                print('gate: 0 of 0 pass or skip, 0 fail, 0.0 s wall, refused by --strict-only (the omission list could not be computed)')
                return 1
        if omitted:
            print(f'[gate] --only leaves out {len(omitted)} lane(s) whose affects match the change: {fg.omitted_text(omitted)}', flush=True)
            if args.strict_only:
                print('FAIL  --strict-only: add those lanes to --only (or pass --base REV if the base is wrong)')
                print(f'gate: 0 of 0 pass or skip, 0 fail, 0.0 s wall, refused by --strict-only, omitted-affected: {len(omitted)}')
                return 1
    elif args.strict_only or args.base:
        print('[gate] note: --strict-only / --base only matter with --only', flush=True)
    t_gate = time.time()   # S102 M4 from here
    all_checks = list(checks)   # S103 DIB: --why reads the lane's checks before --changed filters them
    if args.why and args.changed is None:
        args.changed = ''   # --why CHECK explains the selection against the branch's merge-base
    fast = args.changed is not None
    dtab = None   # S103 DIB S2: ONE derived table per gate (selection and keys share it; refresh() after a build moves the manifests)
    # S103 H3 (audit 1.3): the result cache is on for every mode (--only too) unless --no-cache; --jobs defaults to 4 when every selected check is
    # cacheable (has "affects", not "cache": false) or "parallel_safe": true, else 1. serial_needed() still puts a .ps1 build / no-affects lane on one serial
    # worker (the exe-relink collision rule), so a default of 4 never runs two relinks of one exe side by side.
    jobs = args.jobs or gsched.default_jobs()   # S103 PS2: serial_needed() is gone from the gate: conflicts are decided per path (gsched), so jobs means jobs
    use_cache = not args.no_cache
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
        use_derived = os.environ.get('GATE_NO_DERIVED') != '1' or bool(args.why)   # S103 DIB: selection = union(affects, derived deps); GATE_NO_DERIVED=1 = the old affects-only rule
        dtab = gdeps.Table(root, MAIN, log=lambda m: print(m, flush=True)) if use_derived else None
        sel_why = {}
        keep = []
        for c in checks:
            ok_, why_ = fg.selected(c, changed, c['_lane_rel'], dtab.get(c) if dtab else None, dtab.idx if dtab else None, root)
            sel_why[c['name']] = why_
            if ok_:
                keep.append(c)
        checks = keep
        if dtab:
            dtab.save()
            if dtab.idx is not None and not args.why:
                dtab.idx.close()   # QTF round 3: lane selection is done: do not hold the ours index open for the whole gate (it reopens on demand for the keys)
        print(f"[gate] --changed {base}: {len(changed)} changed path(s); {len(checks)} of {before} checks selected "
              f"({len({c['lane'] for c in checks})} lanes)" + (f"; derived deps: {dtab.hits} cached, {dtab.misses} computed, index {'yes' if dtab.idx else 'MISSING (affects only)'}" if dtab else ''), flush=True)
        if args.why:
            for c in [c for c in all_checks if c['name'] == args.why or c['lane'] == args.why]:
                print(f"[why] {c['name']}: {'SELECTED' if c in checks else 'skipped'}: {sel_why.get(c['name'], '?')}")
                for ln in gdeps.explain(c, dtab.get(c), changed, next((p for p in changed if fg.lane_affects(c) and fg.matches(p, fg.lane_affects(c))), None), dtab.idx, root):
                    print('[why] ' + ln)
            return 0
        if all_files is None and checks:
            all_files = fg.tree_files(root)
        all_files = all_files if all_files is not None else []
    tier_skipped = []   # S103 GST: the rig tier leaves the per-piece gate (CLAUDE.md "The gate is CHEAP checks")
    if args.tier != 'all':
        def _gpu_of(c):
            if c.get('retired') or c.get('parallel_safe') is True:
                return False
            try:
                return gsched.footprint(c, root, MAIN, [])['gpu']
            except Exception:
                return True   # cannot tell: not a cheap check
        checks, tier_skipped = gtiers.split(checks, args.tier, only, _gpu_of)
        if tier_skipped:
            print(f"[gate] --tier {args.tier}: {len(tier_skipped)} rig-tier check(s) skipped (name one in --only as lane.check, or --tier all, to run it): "
                  + ', '.join(c['name'] for c in tier_skipped[:12]) + (f', +{len(tier_skipped) - 12} more' if len(tier_skipped) > 12 else ''), flush=True)
    if args.list:
        for c in checks:
            print(f"{c['lane']:12} {c['name']:28} {' '.join(c['cmd'])}")
        return 0
    if args.footprints:
        reg0 = gexes.registry()
        nunk = ngpu = 0
        for c in checks:
            fp = gsched.footprint(c, root, MAIN, [] if args.no_exes else gexes.infer(c, root, reg0)) if c.get('parallel_safe') is not True else \
                {'reads': set(), 'writes': set(), 'unknown': False, 'gpu': False, 'why': 'parallel_safe: true'}
            nunk += fp['unknown']
            ngpu += fp['gpu']
            kk, why = ('', 'retired') if c.get('retired') else fg.cache_key_ex(root, c, c['_lane_bytes'], [], all_files, MAIN, None,
                                                                              'x' if fg.pk.is_plant(c) and fg.pk.pair_real(c, c['_lane_all']) else None)
            print(f"{c['lane']:12} {c['name']:36} {'UNKNOWN-WRITES(barrier)' if fp['unknown'] else 'writes ' + ','.join(sorted(fp['writes']))[:90]:60} "
                  f"{'GPU ' if fp['gpu'] else ''}{'cacheable' if kk else 'uncached: ' + why}  [{fp['why']}]")
        print(f'{len(checks)} checks; {nunk} with unknown writes (barriers), {ngpu} take a GPU slot')
        return 0
    if not checks and not fast and not tier_skipped:
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
    t_exe = time.time()   # S103 H3: the exe step is timed and written as an "(exe step)" row in times.csv
    for c in checks:
        c['_exes'] = []
        if c.get('retired') or [n for n in c.get('needs', []) if not (MAIN / n).exists()] or (not args.assume_main and gnm.missing(c, root, MAIN)):   # S103 GRF: a retired check builds no exe either; WTG: nor a NEEDS-MAIN one
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
            elif sp['path'] not in exe_state and deadline_at is not None and time.time() >= deadline_at:
                exe_state[sp['path']] = ('DEADLINE', f"{sp['path']}: not built, the --deadline passed")   # S103 GST review 14: no build starts after the deadline (the checks are NOT RUN)
                print(f"[gate] {exe_state[sp['path']][1]}", flush=True)
            elif sp['path'] not in exe_state:
                exe_state[sp['path']] = gexes.ensure(root, sp, cache=tcache)
                print(f"[gate] {exe_state[sp['path']][1]}" + ('  <- BUILD FAILED' if exe_state[sp['path']][0] == 'BUILD' else ''), flush=True)
            c['_exes'].append(sp['path'])
    exe_wall = time.time() - t_exe
    exe_built = [v[1] for v in exe_state.values() if v[0] in ('BUILD', 'STALE') or 'built fresh' in v[1]]
    cache_path = (Path(args.out) if args.out else MAIN / 'out' / 'gate') / 'cache.json'
    cache = {}
    if use_cache:
        try:
            cache = fg.load_cache(cache_path)
        except Exception as e:   # S103 GST review 4: only a MISSING file is empty; an unreadable one is said, and saves then skip rather than overwrite it
            print(f'[gate] could not read {cache_path}: {e} (no cache hits this run; entries are saved only when the file can be read)', flush=True)
    dirty = {}   # S103 GST review 6: what THIS gate adds; the saves write these only, never the whole start-time copy
    cache_lock = threading.Lock()
    out_dir = Path(args.out) if args.out else MAIN / 'out' / 'gate'   # S103 GST: rows and cache entries are written AS checks finish (a killed gate keeps them)
    out_dir.mkdir(parents=True, exist_ok=True)
    mode = f'changed:{args.changed}' if fast else 'full'
    times_lock = threading.Lock()
    tc_lock = threading.Lock()
    print_lock = threading.Lock()

    def glog(m):
        with print_lock:   # S103 GST review 13: lock-wait and save-error lines never interleave with a row
            print(m, flush=True)

    def build_identity(c):
        bs = gtc.build_script_of(c['cmd'])
        return bs[0] + '|' + ' '.join(bs[1])

    def tool_key(c):
        # S103 GX: a check that is just a tools\...\build*.ps1 run (the rig / unit-test exes). Key = script + flags + the exact files the last build of it was
        # compiled from (S103 DIB: its dependency manifest: content + directory names), else the whole src\ + tools\ tree; + toolchain.
        bs = gtc.build_script_of(c['cmd']) if tcache is not None else None
        if bs is None:
            return None
        name = Path(bs[0]).stem + ('_' + '_'.join(a.strip('-').lower() for a in bs[1]) if bs[1] else '')
        with tc_lock:
            return name, tcache.key(build_identity(c))

    def tool_hit(c):
        tool_hit.msg = ''
        nk = tool_key(c)
        if nk is None:
            return False
        got = tcache.restore(*nk)
        if not got:
            return False
        tool_hit.msg = f'tool {nk[0]}: cache hit {nk[1][:8]} ({len(got)} files copied, build not run)'
        return True

    def tool_publish(c, before, tail, fp=None):
        nk = tool_key(c)
        new, changed = gtc.new_outputs(before, gtc.scan_outputs(root))
        # S103 DIB: the dependency manifests (<exe>.deps.txt, out\deps\<script>.deps.txt) are rewritten by every link, so they are never "pre-existing outputs
        # that changed"; they belong to this build when their exe is in its write footprint (or the script's own out\deps file), and travel with the entry.
        dep_files = [p for p in new + changed if p.endswith('.deps.txt')]
        new, changed = [p for p in new if not p.endswith('.deps.txt')], [p for p in changed if not p.endswith('.deps.txt')]
        if nk is None or not new:
            return tail
        # S103 PS2: builds run side by side now, so "every exe that appeared since I started" may include another check's. Only outputs inside this check's
        # own (locked) write footprint are attributed to it; one inside ANOTHER check's footprint is theirs and ignored; one inside nobody's (or an unknown
        # footprint) means we cannot tell whose it is: this entry is not published (the next gate rebuilds), never published wrong.
        if fp is None or fp['unknown'] or not fp['writes']:
            return tail + f'  [tool {nk[0]}: not cached, its write footprint is unknown]'
        mine = lambda p: gsched.owns(p, fp['writes'])   # S103 GST review 1: <stem>.pdb / .ilk / .exp / .lib of a written exe are this build's
        foreign = [p for p in new + changed if not mine(p) and not gsched.owns(p, all_writes)]
        if foreign:
            return tail + f'  [tool {nk[0]}: not cached, {len(foreign)} output(s) outside every write footprint]'
        new = [p for p in new if mine(p)]
        changed = [p for p in changed if mine(p)]
        if not new:
            return tail
        if changed:   # a file that already existed changed: it may belong to an earlier build, so this entry could be partial. Not cached.
            return tail + f'  [tool {nk[0]}: not cached, {len(changed)} pre-existing output(s) rewritten]'
        sid = gdeps.script_id(gtc.build_script_of(c['cmd'])[0]) + '.deps.txt'
        dep_mine = [p for p in dep_files if (p.startswith('out/deps/') and p == 'out/deps/' + sid) or (not p.startswith('out/deps/') and mine(p[:-len('.deps.txt')]))]
        new = new + dep_mine
        identity = build_identity(c)
        with tc_lock:
            nk = (nk[0], tcache.rekey(identity, new))   # the key from the manifest this build just wrote (the whole-tree key when it wrote none)
        if tcache.publish(nk[0], nk[1], new):
            return tail + f'  [tool {nk[0]}: built, cached {nk[1][:8]} ({tcache.how.get(identity, "tree")} key)]'
        return tail

    # ---- S103 PS2: what each check touches, the GPU slots, the proof cache's pairing and audit -------------------------------------------------------------
    sel_ids = {id(c) for c in checks}
    for c in checks:
        c['_fp'] = ({'reads': set(), 'writes': set(), 'unknown': False, 'gpu': False, 'why': 'parallel_safe: true'} if c.get('parallel_safe') is True
                    else gsched.footprint(c, root, MAIN, c['_exes']))
    gpu_n = gsched.default_gpu_slots() if args.gpu_slots < 0 else args.gpu_slots
    slots = gsched.GpuSlots(Path(os.environ.get('GATE_SLOTS_DIR') or (MAIN / 'out' / 'gate' / 'gpu_slots')), gpu_n, log=lambda m: print(m, flush=True), who={'root': str(root), 'gate': RUN_ID}) if gpu_n > 0 and any(c['_fp']['gpu'] for c in checks) else None
    plocks = gsched.PathLocks()
    cstate = {}   # S103 GST: check name -> (state, since, detail) for the stall watchdog: 'paths' | 'slots' | 'running'
    last_progress = {'t': time.time()}
    all_writes = set().union(*[c['_fp']['writes'] for c in checks]) if checks else set()
    try:
        verify_p = args.verify_p if args.verify_p >= 0 else float(os.environ.get('GATE_VERIFY_P', '0.05'))
    except ValueError:
        verify_p = 0.05
    stamp = gtc.toolchain_stamp(root) if use_cache else None
    gpk.set_memo((Path(args.out) if args.out else MAIN / 'out' / 'gate') / 'hashmemo.json')
    key_tab = None
    if use_cache and os.environ.get('GATE_NO_DERIVED') != '1':
        key_tab = dtab if dtab is not None else gdeps.Table(root, MAIN, log=lambda m: print(m, flush=True))
        key_tab.refresh()   # the exe step above may have rewritten manifests
    key_tab_mu = threading.Lock()
    key_manifest = {}    # id(check) -> True when its key holds build manifests (re-keyed after the check ran: S103 DIB2 review bug 5)
    key_memo = {}        # id(check) -> (key, why): the key a check had when it was computed (a planted-fault twin reads its real check's)
    pend_plants = []     # (twin result name, twin key, real check, real key): written at the end when the real check passed under that very key
    audit = {'hits': 0, 'verified': 0, 'mismatch': 0, 'dropped': []}
    wait = {'path_s': 0.0, 'gpu_s': 0.0}
    chk_root = root / 'out' / 'gate' / 'chk'

    try:
        key_timeout = float(os.environ.get('GATE_KEY_TIMEOUT', '120'))
    except ValueError:
        key_timeout = 120.0
    # GKL: a check's cache key is computed at most ONCE per attempt. kstat = check name -> [computes, total seconds, last seconds] (the stall report prints it); write_epoch
    # counts the finished checks that wrote (or may have written) something: the key taken before a GPU-slot wait is reused after it while the epoch has not moved.
    kstat = {}
    write_epoch = {'n': 0}

    def keyed(cc, fresh=False):
        """DPC: compute_key under a bound. While it runs the check's state is 'key' (the stall report names it); an index / sqlite access that does not return in
        GATE_KEY_TIMEOUT s (default 120) gives up: key None = uncacheable, the check just runs (S103 QX4: qd_decomp.selftest hung 25 min in compute_key, 'blocked by ?')."""
        name = cc['name']
        cstate[name] = ('key', time.time(), '')
        res = {}

        def work():
            try:
                res['v'] = compute_key(cc, fresh)
            except BaseException as e:
                res['e'] = e
        t_key = time.time()
        th = threading.Thread(target=work, daemon=True, name='gate-key-' + name)
        th.start()
        th.join(key_timeout)
        cstate[name] = ('paths', time.time(), '')
        ks = kstat.setdefault(name, [0, 0.0, 0.0])
        ks[0] += 1
        ks[2] = time.time() - t_key
        ks[1] += ks[2]
        if th.is_alive():
            with print_lock:
                print(f'[gate] {gsched.hms()} cache key of {name} did not finish in {key_timeout:g} s (index / sqlite busy?): it runs uncached', flush=True)
            return (None, f'cache key timed out after {key_timeout:g} s: ran uncached')
        if 'e' in res:   # DPC: a key that cannot be computed (a file locked / unreadable) makes the check uncacheable, it does not kill the lane
            e = res['e']
            with print_lock:
                print(f'[gate] {gsched.hms()} cache key of {name} failed ({type(e).__name__}: {str(e)[:120]}): it runs uncached', flush=True)
            return (None, f'cache key failed: {type(e).__name__}')
        return res['v']

    def compute_key(cc, fresh=False):
        if os.environ.get('GATE_TEST_KEY_COUNT_FILE') and os.environ.get('GATE_TEST_KEY_COUNT') == cc['name']:   # selftest plant (sched_selftest GKL): one line per compute, and a slow key
            with open(os.environ['GATE_TEST_KEY_COUNT_FILE'], 'a') as cf:
                cf.write('key\n')
            time.sleep(float(os.environ.get('GATE_TEST_KEY_SLOW_S', '0')))
        if os.environ.get('GATE_TEST_KEY_RAISE') == cc['name']:   # selftest plant (sched_selftest DPC2): the key read hits a locked file
            raise PermissionError(13, 'Permission denied (planted)')
        if os.environ.get('GATE_TEST_KEY_HANG') == cc['name']:   # selftest plant (deps_selftest): a key computation that never returns
            time.sleep(float(os.environ.get('GATE_TEST_KEY_HANG_S', '60')))
        # fresh = under the check's path locks (S103 PS2 review F4): every folder some check of this gate may write is read again, never taken from an earlier look
        if fresh and all_writes:
            gpk.invalidate([b / re.sub(r'^@main/', '', w) for w in all_writes for b in (root, MAIN)])
        progs = fg.program_list(cc)
        prog_paths = [e for e in cc.get('_exes', []) if exe_state.get(e, ('',))[0] != 'STALE'] + [pr['path'] for pr in progs]
        pair_key = None
        if gpk.is_plant(cc):
            real = gpk.pair_real(cc, cc['_lane_all'])
            if real is not None and id(real) in sel_ids:
                pair_key = (None if fresh else key_memo.get(id(real), (None,))[0]) or compute_key(real, fresh)[0]
        dv = None
        if key_tab is not None:
            with key_tab_mu:
                if fresh:
                    key_tab.refresh()   # a check of this gate may have rebuilt an exe (new manifests): the derived entries that read them are looked at again
                dv = key_tab.get(cc)
        fpc = cc.get('_fp') or {}
        meta = {}
        k = fg.cache_key_ex(root, cc, cc['_lane_bytes'], prog_paths, all_files, MAIN, stamp, pair_key, derived=dv,
                            pure=bool(fpc) and not fpc.get('writes') and not fpc.get('unknown'), meta=meta)   # S103 DIB: derived deps key an exact, write-free check
        key_manifest[id(cc)] = bool(meta.get('manifest'))
        if k[0] is None and gpk.is_plant(cc) and pair_key is None and 'paired' in k[1]:
            k = (None, 'planted-fault twin: its paired real check is not in this gate' if gpk.pair_real(cc, cc['_lane_all']) else k[1])
        key_memo[id(cc)] = k
        return k

    def tree_moved():
        if head0 is None:
            return False
        now = head_of(root)
        if now is not None and now != head0:
            with print_lock:
                if moved['to'] != now:
                    print(f'[gate] TREE MOVED: HEAD was {head0[:10]} at start, is {now[:10]} now (a merge / checkout while the gate runs): results from here on are marked, never cached', flush=True)
            moved['to'] = now
            moved['n'] += 1
            return True
        return False

    def finish_row(c, verdict, tail, dt, waited, label):
        """The one place a check's row is made: printed with its clock time (S103 GST: @HH:MM:SS after the seconds; the verdict column stays first for gate_wait / gate_main), appended to
        times.csv at once and counted as progress for the stall watchdog, so a gate that is killed keeps every finished row."""
        res = {'lane': c['lane'], 'name': c['name'], 'verdict': verdict, 'seconds': round(dt, 1), 'last': tail, 'exes': c.get('_exes', []), 'cache': label,
               'wait_s': round(waited, 1), 'gpu': c['_fp']['gpu'], 'writes_unknown': c['_fp']['unknown']}   # S103 audit: hit / miss / uncacheable / off; PS2: hit-verified / hit-MISMATCH, waits
        with print_lock:
            print(f"{verdict:5} {c['lane']:12} {c['name']:28} {dt:6.1f} s  @{time.strftime('%H:%M:%S')}  " + (f'(waited {waited:.1f} s)  ' if waited >= 0.5 else '') + (tail[:150] if verdict == 'PASS' else tail), flush=True)   # S103 GSP: a failing check's last lines are printed whole (they were clipped to 150 characters)
            last_progress['t'] = time.time()
        try:
            with times_lock:
                fg.append_times(out_dir / 'times.csv', [[time.strftime('%Y-%m-%d %H:%M:%S'), str(root), mode, c['lane'], c['name'], verdict, round(dt, 1), label, RUN_ID]])
        except (OSError, TimeoutError) as e:
            glog(f'[gate] could not append times.csv: {e}')
        return res

    min_left = float(os.environ.get('GATE_MIN_LEFT_S') or 10)
    last_s = fg.last_seconds(out_dir / 'times.csv') if deadline_at is not None else {}

    def time_short(c):
        # S103 GST review 3: a check is not STARTED with less than ~10 s (or its last run's seconds in times.csv) left before the --deadline: it would only be killed and counted a failure.
        left = deadline_at - time.time()
        need = max(min_left, last_s.get(c['name'], 0.0))
        if left < need:
            return f'not started: {left:.0f} s left before the --deadline of {args.deadline:.0f} s, this check needs ~{need:.0f} s (run it again)'
        return None

    def run_check(c):
        if deadline_at is not None and time.time() >= deadline_at:
            return finish_row(c, 'NOT RUN', f'--deadline {args.deadline:.0f} s passed before this check started', 0.0, 0.0, 'off')
        retired = c.get('retired')   # S103 GRF: "retired": "reason" = the check tests a path that no longer ships; it is a SKIP with the reason, never a pass, and never runs
        missing = [] if retired else [n for n in c.get('needs', []) if not (MAIN / n).exists()]
        nmain = [] if retired or missing or args.assume_main else gnm.missing(c, root, MAIN)   # S103 WTG: what this tree lacks and MAIN has (root_needs / implicit): reported at once
        t0 = time.time()
        bad = [exe_state[e][1] for e in c['_exes'] if exe_state[e][0] == 'BUILD']
        stale = [exe_state[e][1] for e in c['_exes'] if exe_state[e][0] == 'STALE']
        progs = fg.program_list(c)   # S102 M4: programs the check declares must be as new as their sources, in every mode
        for pr in progs:
            why = fg.stale_program(root, pr, all_files)
            if why:
                stale.append(f'stale build: {why}')
        key, why_nokey, hit, verify, cached, pair_key = None, '', False, False, None, None
        cacheable = use_cache and not retired and not missing and not nmain and not bad and not stale
        waited = 0.0
        if retired:
            verdict, tail = 'SKIP', 'retired: ' + str(retired)
        elif missing:
            verdict, tail = 'SKIP', 'missing: ' + ', '.join(missing)
        elif nmain:
            verdict, tail = 'NEEDS-MAIN', 'not run here: this tree lacks ' + ', '.join(nmain) + ' (the main checkout has it): hand it to the main session (gate_main.py)'
        elif tree_moved():
            verdict, tail = 'FAIL', f'tree moved: HEAD was {head0[:10]} at gate start and is {moved["to"][:10]} now; this check was not run on a consistent tree (re-run the gate)'
        elif bad:
            verdict, tail = 'BUILD', bad[0] + ' (check not run)'
        elif stale:
            verdict, tail = 'BLOCKED', stale[0] + ' (check not run)'   # S105 E: could not run is BLOCKED, never FAIL (the change is not wrong)
        else:
            # take what the check needs: a GPU slot first (always first: a holder of path locks never waits for a slot, so no cycle), then its paths. The proof
            # cache's key is computed UNDER the path locks (S103 PS2 review F4): a check that rebuilds an exe or rewrites a folder this one reads has finished
            # (or not started) when the key is taken, so a hit can never be a stale PASS and a stored PASS carries the inputs the check really ran with.
            fp = c['_fp']
            need_gpu = slots is not None and fp['gpu']
            gtok = ptok = None
            audit_roll = args.verify_cache or random.random() < verify_p
            slot_waited = False   # GKL fix: after a REAL slot wait the key is read again (slots are shared across gates: a foreign GPU check may have written MAIN's out folders); only the PathsBusy turn reuses it
            key_got = None   # GKL: (write_epoch at the compute, key, why) of this attempt's one key compute
            tw = time.time()
            tg = tw
            notrun = False
            cstate[c['name']] = ('paths', tw, '')
            try:   # DPC: any exception between taking the paths / a slot and the run (a cache key that raises PermissionError, QX4 / RFL 2026-11-19) must not leak them:
                   # the lane thread died holding its locks and every check queued behind it waited out the deadline
                while True:
                    try:
                        ptok = plocks.acquire(fp, c['name'], log=glog, until=deadline_at, nowait=gtok is not None)
                    except gsched.WaitDeadline:
                        notrun = True
                        break
                    except gsched.PathsBusy:
                        # S103 GST review 8: a slot holder never WAITS for paths (no idle-held GPU slot behind a queued writer): let the slot go and wait for the paths with nothing held
                        slots.release(gtok)
                        gtok = None
                        cstate[c['name']] = ('paths', time.time(), '')
                        continue
                    if cacheable:
                        if key_got is not None and key_got[0] == write_epoch['n'] and not slot_waited:
                            # GKL: the key was computed under the paths of an earlier turn of this loop (a miss that needed a GPU slot gave them up to wait for it). It is
                            # still this check's key while no check of this gate that writes has finished since: every folder a check of this gate may write is in
                            # all_writes, and a foreign writer would change the key between the compute and the run just the same (the key is a snapshot before the run).
                            key, why_nokey = key_got[1], key_got[2]
                        else:
                            n0 = write_epoch['n']
                            key, why_nokey = keyed(c, fresh=True)
                            key_got = (n0, key, why_nokey)
                        hit = key is not None and cache.get(key, {}).get('verdict') == 'PASS'
                        if hit and gpk.is_plant(c):
                            # a planted-fault twin is served only together with its real check: the real check's PASS under the same key must be there too
                            real = gpk.pair_real(c, c['_lane_all'])
                            pair_key = key_memo.get(id(real), (None,))[0] if real is not None else None
                            hit = pair_key is not None and cache.get(pair_key, {}).get('verdict') == 'PASS'
                        if hit:
                            cached = dict(cache[key])
                            if audit_roll:
                                verify, hit = True, False   # re-run it for real below; compare with the cached PASS
                    if hit or not need_gpu or gtok is not None:
                        break
                    plocks.release(ptok)   # a miss that needs the GPU: wait for a slot WITHOUT holding paths, then take the paths again and read the key again
                    ptok = None
                    cstate[c['name']] = ('slots', time.time(), '')
                    t_rel = time.time()
                    try:
                        gtok = slots.acquire(c['name'], n=int(c.get('gpu_procs', 1)), until=deadline_at)   # S103 GSP2: gpu_procs N = N slots at once
                    except gsched.WaitDeadline:
                        notrun = True
                        break
                    cstate[c['name']] = ('paths', time.time(), '')
                    slot_waited = slot_waited or (time.time() - t_rel) >= 1.0   # a slot that was free at once is no wait
                    tg = time.time()
            except BaseException:
                cstate.pop(c['name'], None)
                if ptok is not None:
                    plocks.release(ptok)
                if gtok is not None:
                    slots.release(gtok)
                raise
            tp = time.time()
            if notrun:
                cstate.pop(c['name'], None)
                if gtok is not None:   # S103 GST review 2: the deadline passed in the second paths wait: the slot goes back
                    slots.release(gtok)
                return finish_row(c, 'NOT RUN', f'--deadline {args.deadline:.0f} s passed while this check waited for paths or a GPU slot', tp - t0, tp - t0, 'off')
            cstate[c['name']] = ('running', tp, '')
            waited = tp - tw
            with print_lock:
                wait['gpu_s'] += tg - tw
                wait['path_s'] += tp - tg
                if hit or verify:
                    audit['hits'] += 1
            try:
                short = None if hit or deadline_at is None else time_short(c)
                if hit:
                    verdict, tail = 'PASS', 'cache hit (same lane json, inputs and programs as the passing run of ' + cache[key].get('when', '?') + ')'
                elif short:
                    verdict, tail, key = 'NOT RUN', short, None
                else:
                    verdict, tail = run_body(c, fp)
                    if verdict == 'PASS' and key is not None and key_manifest.get(id(c)) and not gpk.is_plant(c):
                        # bug 5: the key was taken BEFORE the run from the manifest of an earlier build; the run (a build) wrote a new one. The PASS is stored under the
                        # key the next lookup will compute: from the manifests this run wrote (still under the check's path locks).
                        key2, _w2 = keyed(c, fresh=True)
                        key = key2   # None = not cacheable after all: nothing is stored
            finally:
                cstate.pop(c['name'], None)
                if fp['writes'] or fp['unknown']:
                    with print_lock:
                        write_epoch['n'] += 1
                plocks.release(ptok)
                if gtok is not None:
                    slots.release(gtok)
        if verdict == 'PASS' and not hit and not retired and tree_moved():   # moved WHILE this check ran: its PASS is not on one tree, never cached
            verdict, tail, key = 'FAIL', f'tree moved while it ran: HEAD {head0[:10]} -> {moved["to"][:10]} (result not trusted; re-run the gate)', None
        dt = time.time() - t0 - waited
        label = 'off' if verdict == 'NOT RUN' else 'hit' if hit else ('miss' if key is not None else ('uncacheable: ' + why_nokey if use_cache and why_nokey else ('uncacheable' if use_cache else 'off')))
        if verify:
            if verdict == 'PASS':
                audit['verified'] += 1
                label, tail = 'hit-verified', tail + '  [cache hit re-run for real: same verdict, ' + str(cached.get('when', '?')) + ']'
            else:
                audit['mismatch'] += 1
                audit['dropped'].append(key)
                tail = (f'CACHE MISMATCH: the cache said PASS (key {key[:12]}, from {cached.get("when", "?")}, check {cached.get("check", "?")}) but a re-run for real gives '
                        f'{verdict}: {tail}')
                verdict, label = 'FAIL', 'hit-MISMATCH'
                with cache_lock:
                    cache.pop(key, None)
                    dirty.pop(key, None)
                    try:
                        fg.save_cache(cache_path, {}, drop=[key])   # S103 GST review 6: a wrong entry leaves the file AT ONCE (a killed gate would keep it otherwise)
                    except Exception as e:
                        glog(f'[gate] could not drop the wrong cache entry now: {e} (it is dropped at the end)')
        if key is not None and verdict == 'PASS' and not hit and not verify:
            if gpk.is_plant(c):
                real = gpk.pair_real(c, c['_lane_all'])
                with cache_lock:
                    pend_plants.append((c['name'], key, real, key_memo.get(id(real), (None,))[0] if real is not None else None))
            else:
                with cache_lock:
                    cache[key] = {'verdict': 'PASS', 'when': time.strftime('%Y-%m-%d %H:%M'), 'check': c['name']}
                    dirty[key] = cache[key]
                    try:
                        fg.save_cache(cache_path, {key: cache[key]})   # S103 GST: this PASS survives a kill (merged into the file under cache.json.lock, replaced atomically); the end-of-gate save adds the rest
                        dirty.pop(key, None)
                    except Exception as e:
                        glog(f'[gate] could not save a cache entry: {e}')
        return finish_row(c, verdict, tail, dt, waited, label)

    # ---- S103 DIB: one shared game_syntax run for every game_syntax check of this gate (tools\gate\gs_share.py) ---------------------------------------------
    gs_on = os.environ.get('GATE_NO_SHARE') != '1'
    gs_parsed = {id(c): gsh.parse(c['cmd']) for c in checks if gs_on and not c.get('retired')}
    gs_state, gs_mu = {'res': None}, threading.Lock()

    def gs_shared_run():
        with gs_mu:   # the first shared check runs it; the others wait here and read the same result
            if gs_state['res'] is None:
                members = [x for x in checks if gs_parsed.get(id(x)) and not x.get('retired') and not [n for n in x.get('needs', []) if not (MAIN / n).exists()]]
                files, twin = gsh.union([gs_parsed[id(x)] for x in members])
                cmd = gsh.shared_cmd(files, twin)
                tmo = nat = max([x.get('timeout_s', 300) for x in members] + [300])
                if deadline_at is not None:
                    tmo = max(1, min(tmo, int(deadline_at - time.time()) + 1))   # S103 GST review 14
                t0 = time.time()
                try:
                    p = run_tree(_resolve_cmd(cmd, root), root, tmo, dict(os.environ, RDR_MAIN_CHECKOUT=str(MAIN)))
                    gs_state['res'] = (p.returncode, p.stdout + p.stderr, time.time() - t0, len(members), cmd)
                except subprocess.TimeoutExpired:
                    gs_state['res'] = (124, f'killed at the --deadline after {tmo} s (run it again)' if tmo < nat else f'timeout after {tmo} s', time.time() - t0, len(members), cmd)
                except FileNotFoundError as e:
                    gs_state['res'] = (127, f'cannot start: {e}', time.time() - t0, len(members), cmd)
                print(f"[gate] shared game_syntax: ONE run for {len(members)} check(s), {len(gs_state['res'][4])} argv words, {gs_state['res'][2]:.1f} s", flush=True)
            return gs_state['res']

    # groups of checks with the identical command that may share one run: no build script (tool cache / publish), no GATE_CHECK_DIR in the script closure (the
    # per-check scratch folder would differ), not the game_syntax share, not retired
    same_run = {}
    if os.environ.get('GATE_NO_SHARE') != '1':
        by_cmd = {}
        for c in checks:
            if c.get('retired') or gs_parsed.get(id(c)) or gtc.build_script_of(c['cmd']) or not c['cmd']:
                continue
            by_cmd.setdefault(json.dumps(c['cmd']), []).append(c)
        for lst in by_cmd.values():
            if len(lst) < 2:
                continue
            txt = ''
            for a in lst[0]['cmd']:
                if isinstance(a, str) and a.lower().endswith(('.py', '.ps1')):
                    txt += ''.join(t for _p, _k, t, _top in gsched.closure_files(root, a.replace('\\', '/')))
            if 'GATE_CHECK_DIR' in txt:
                continue
            grp = {'mu': threading.Lock(), 'res': None, 'who': ''}
            for c in lst:
                same_run[id(c)] = grp

    def run_body(c, fp):
        # the check itself, under the locks it needs: the tool-exe cache restore, or the command
        if gs_parsed.get(id(c)):
            rc, text, _s, _n, _cmd = gs_shared_run()
            if rc == 124 and text.startswith('killed at the --deadline'):
                return 'NOT RUN', text
            return gsh.verdict(c['cmd'], rc, text)
        if tool_hit(c):
            return 'PASS', tool_hit.msg
        grp = same_run.get(id(c))
        if grp is not None:
            # S103 DIB2 S3: checks of different lanes with the SAME argv (one gate = one tree, one environment) are ONE run: d3_arena.unit and d3_wrestlers.unit ran the
            # identical ~400 s command twice. The first runs it; the others wait here and take its verdict and output.
            with grp['mu']:
                first = grp['res'] is None
                if first:
                    grp['res'] = run_cmd(c, fp)
                    grp['who'] = c['name']
                v, t = grp['res']
                return v, (t if first else t + f"  [same command as {grp['who']}: one shared run]")
        return run_cmd(c, fp)

    def run_cmd(c, fp):
        env = dict(os.environ, RDR_MAIN_CHECKOUT=str(MAIN))
        t_run0 = time.time()
        if c.get('gpu_procs'):   # S103 GSP2: a check that runs several GPU processes at once holds that many slots and is told its share
            env['GATE_GPU_PROCS'] = str(min(int(c['gpu_procs']), gpu_n) if gpu_n > 0 else int(c['gpu_procs']))
        # S103 PS2: this check's own scratch folder in THIS checkout (a picture, a dump): never shared with another check or another worktree. Emptied first.
        cd = chk_root / re.sub(r'[^A-Za-z0-9_.-]', '_', c['name'])
        try:
            if cd.exists():
                shutil.rmtree(cd, ignore_errors=True)
            cd.mkdir(parents=True, exist_ok=True)
            env['GATE_CHECK_DIR'] = str(cd)
        except OSError:
            pass
        tc_before = gtc.scan_outputs(root) if tcache is not None and gtc.build_script_of(c['cmd']) else None
        try:
            # S103 GSP: a check that uses the GPU (a rig, a proof) runs beside other GPU checks when the gate has several jobs: its timeout is doubled then (P7H1.rig and D.proof each timed out once under contention and passed alone)
            per = c.get('timeout_s', 300)
            tmo = int(per * 1.5) if jobs > 1 and c['_fp']['gpu'] else per   # S103 GST: was doubled (a 2400 s check got 4800 s, past any outer cap); +50 % now
            nat = tmo
            if deadline_at is not None:
                tmo = max(1, min(tmo, int(deadline_at - time.time()) + 1))   # a running check ends at the deadline (to the next whole second): nothing starts after it
            c['_tmo'] = tmo
            c['_clamped'] = tmo < nat
            try:
                p = run_tree(_resolve_cmd(c['cmd'], root), root, tmo, env)
            except FileNotFoundError as e:   # S105 E review 3: only the START of the program is BLOCKED; a later FileNotFoundError (below) stays a FAIL
                return 'BLOCKED', f'cannot start: {e}'
            verdict = 'PASS' if p.returncode == 0 else 'FAIL'
            out = (p.stdout + p.stderr).strip().splitlines()
            tail = out[-1] if out else '(no output)'
            if verdict == 'FAIL':
                tail = ' | '.join(out[-4:])
                try:   # S103 GSP: the whole output of a failing check is kept (out/gate/fail/<check>.log), its path rides on the printed line
                    fd = root / 'out' / 'gate' / 'fail'
                    fd.mkdir(parents=True, exist_ok=True)
                    lg = fd / (re.sub(r'[^A-Za-z0-9_.-]', '_', c['name']) + '.' + RUN_ID + '.log')
                    lg.write_text('\n'.join(out), encoding='utf-8')
                    tail += f'  [full output: {lg}]'
                except OSError:
                    pass
                # S103 WTG: a FAIL that is only a missing file of the MAIN checkout (a worktree): needs_main.reclassify is strict (final traceback line, folder wholly absent, short run, no
                # PASS / FAIL / assertion line of its own). Never under --assume-main. The original FAIL tail rides on the row.
                nm_late = [] if args.assume_main else gnm.reclassify(root, MAIN, out, time.time() - t_run0)
                if nm_late:
                    return 'NEEDS-MAIN', 'died on ' + ', '.join(nm_late) + ', which this tree lacks and the main checkout has (undeclared need: add "root_needs"); original FAIL: ' + tail
            else:   # S103 GSP2: a PASS clears this check's OLDER fail logs (written before this run of it began: a concurrent gate's fresh log stays)
                try:
                    stem = re.sub(r'[^A-Za-z0-9_.-]', '_', c['name']) + '.'
                    for old in (root / 'out' / 'gate' / 'fail').glob(stem + '*.log'):
                        if old.stat().st_mtime < t_run0:
                            old.unlink()
                except OSError:
                    pass
            # a nested build or link error in the check's own output is a BUILD, not a verdict on the code (a PASS exit with a "FAIL ... build"
            # line means the check ran on an exe it could not rebuild: the same)
            joined = '\n'.join(out)
            cause = gexes.build_cause(joined)
            if cause and (verdict == 'FAIL' or re.search(r'^\s*FAIL\b.*\b(fview|renderer|rplay) build\b', joined, re.M | re.I)):
                verdict, tail = 'BUILD', cause
            if verdict == 'PASS' and tc_before is not None:
                tail = tool_publish(c, tc_before, tail, fp)
        except subprocess.TimeoutExpired:
            if c.get('_clamped'):   # S103 GST review 3: killed by the --deadline clamp, not by its own timeout: not a failure of the check
                verdict, tail = 'NOT RUN', f"killed at the --deadline after {c.get('_tmo')} s (still running; run it again)"
            else:
                verdict, tail = 'FAIL', f"timeout after {c.get('_tmo', c.get('timeout_s', 300))} s"
        except FileNotFoundError as e:
            verdict, tail = 'FAIL', f'FileNotFoundError after the check ran: {e}'   # S105 E review 3: not a start failure (run_tree's own is BLOCKED above)
        return verdict, tail

    def run_lane(lane_checks):
        return [run_check(c) for c in lane_checks]   # the checks of one lane keep their order (build, then unit, then proof)

    by_lane = {}
    for c in checks:
        by_lane.setdefault(c['lane'], []).append(c)
    # S103 GST: the stall watchdog. When no check has finished (no row printed) for GATE_STALL_S (default 600 s), say what is in flight and what each waiting check waits for:
    # the 3,600 s main gate that was "stuck" printed nothing for 20-minute stretches, and its waits could not be told from a hang.
    try:
        stall_s = float(os.environ.get('GATE_STALL_S', '600'))
    except ValueError:
        stall_s = 600.0
    stop_wd = threading.Event()

    def stall_report():
        now = time.time()
        snap = dict(cstate)
        run = sorted((n, s) for n, (st, s, _d) in snap.items() if st == 'running')
        pw = sorted(n for n, (st, _s, _d) in snap.items() if st == 'paths')
        sw = sorted(n for n, (st, _s, _d) in snap.items() if st == 'slots')
        kw = sorted((n, s) for n, (st, s, _d) in snap.items() if st == 'key')
        blocked = dict(plocks.blocked_by)
        span = f'{stall_s / 60:g} min' if stall_s >= 60 else f'{stall_s:g} s'
        return (f'[gate] {gsched.hms(now)} no check finished in {span}: running ' + (', '.join(f'{n} (since {gsched.hms(s)[:5]})' for n, s in run) or 'nothing')
                + '; ' + ('; '.join(f'{n}: computing cache key since {gsched.hms(s)[:5]} ({now - s:.0f} s so far, compute #{kstat.get(n, [0])[0] + 1}, {kstat.get(n, [0, 0.0])[1]:.0f} s in earlier ones)' for n, s in kw) + '; ' if kw else '')
                + 'waiting for paths: ' + (', '.join(f"{n} (blocked by {blocked.get(n, '?')})" for n in pw) or 'none')
                + '; waiting for slots: ' + (', '.join(sw) or 'none') + (f'; GPU slots: {slots.holders()}' if slots is not None else ''))

    def watchdog():
        while not stop_wd.wait(min(5.0, max(0.2, stall_s / 4))):
            if time.time() - last_progress['t'] >= stall_s:
                with print_lock:
                    print(stall_report(), flush=True)
                    last_progress['t'] = time.time()   # the next report comes a full window later

    if stall_s > 0:   # GATE_STALL_S <= 0 = off (S103 GST review 14)
        threading.Thread(target=watchdog, daemon=True, name='gate-stall-watchdog').start()
    if jobs <= 1:
        lane_results = [run_lane(v) for v in by_lane.values()]
    else:
        # S103 PS2: every lane runs on the pool; a check waits only for the paths it conflicts on (gsched.PathLocks) and, if it uses the GPU, for a slot.
        lanes = list(by_lane.values())
        with ThreadPoolExecutor(max_workers=max(1, jobs)) as ex:
            futs = [ex.submit(run_lane, v) for v in lanes]
            lane_results = [f.result() for f in futs]
    stop_wd.set()
    # a planted-fault twin's PASS is cached only now, and only when its real check passed under the very key the twin was bound to
    res_by_name = {r['name']: r for lr in lane_results for r in lr}
    for nm, k, real, rk in pend_plants:
        rr = res_by_name.get(real['name']) if real is not None else None
        if rr is not None and rr['verdict'] == 'PASS' and rk is not None and cache.get(rk, {}).get('verdict') == 'PASS':
            cache[k] = {'verdict': 'PASS', 'when': time.strftime('%Y-%m-%d %H:%M'), 'check': nm, 'pair': rk}
            dirty[k] = cache[k]
    results = [r for lr in lane_results for r in lr]
    nnotrun = sum(r['verdict'] == 'NOT RUN' for r in results)
    nblocked = sum(r['verdict'] == 'BLOCKED' for r in results)   # S105 E: could not run (a stale / missing program, a program that would not start)
    failed = sum(r['verdict'] in ('FAIL', 'BUILD', 'NOT RUN', 'BLOCKED') for r in results)   # S103 GST: a check the deadline kept from running is not a pass
    nmain_rows = [r for r in results if r['verdict'] == 'NEEDS-MAIN']
    wall = time.time() - t_gate
    if use_cache:
        try:   # S103 GST review 5: a save error never kills a finished gate before its summary
            if dirty or audit['dropped']:
                fg.save_cache(cache_path, dirty, drop=audit['dropped'])   # only this gate's own new entries (review 6)
            gpk.save_memo()
            if key_tab is not None:
                key_tab.save()
        except Exception as e:
            glog(f'[gate] end-of-gate save failed: {type(e).__name__}: {e} (the rows and the per-check cache entries were already written)')
    now = time.strftime('%Y-%m-%d %H:%M:%S')
    fg.append_times(out_dir / 'times.csv', [[now, str(root), mode, '(exe step)', f'{len(exe_state)} exes, {len(exe_built)} built/stale', 'PASS', round(exe_wall, 1), '', RUN_ID]]
                    + [[now, str(root), mode, '(whole gate)', f'{len(results)} checks, {jobs} jobs', 'FAIL' if failed else 'PASS', round(wall, 1), '', RUN_ID]])
    (out_dir / 'last.json').write_text(json.dumps({'when': time.strftime('%Y-%m-%d %H:%M'), 'run': RUN_ID, 'tier': args.tier, 'rig_skipped': [c['name'] for c in tier_skipped], 'root': str(root), 'jobs': jobs, 'gpu_slots': gpu_n, 'cache_audit': {k: v for k, v in audit.items() if k != 'dropped'}, 'waits_s': {k: round(v, 1) for k, v in wait.items()}, 'results': results}, indent=1), encoding='utf-8')
    nbuild = sum(r['verdict'] == 'BUILD' for r in results)
    print(f"gate: {len(results) - failed} of {len(results)} pass or skip, {failed} fail"
          + (f" ({nbuild} of them BUILD: a tool exe did not build, not a verdict on the code)" if nbuild else '')
          + (f" ({nblocked} of them BLOCKED: the check could not run, not a verdict on the code)" if nblocked else '')
          + (f" ({nnotrun} of them NOT RUN: the --deadline of {args.deadline:.0f} s passed; run those again, the finished rows are cached)" if nnotrun else '')
          + (f", {len(nmain_rows)} needs-main" if nmain_rows else '') + f", {wall:.1f} s wall" + (f", rig skipped: {len(tier_skipped)}" if args.tier != 'all' else '')
          + (f", omitted-affected: {len(omitted)}" if omitted is not None else ', omitted-affected: ?' if omit_err else ''))
    if nmain_rows:   # S103 WTG: the builder hands this list back; the main session runs them (tools\gate\gate_main.py --rev <commit> --only <rows>)
        print('needs-main: ' + ', '.join(r['name'] for r in nmain_rows), flush=True)
    print(f"[gate] {jobs} jobs, {gpu_n} GPU slots; waited {wait['path_s']:.0f} s on path conflicts, {wait['gpu_s']:.0f} s on GPU slots (summed over checks); "
          f"cache: {audit['hits']} hits, {audit['verified']} re-run to verify (p={1.0 if args.verify_cache else verify_p}), {audit['mismatch']} MISMATCH", flush=True)
    if audit['mismatch']:
        print(f"FAIL  cache audit: {audit['mismatch']} cached PASS result(s) did not reproduce; those entries were dropped. A cached result was WRONG: the key is missing an input.")
    if nmain_rows and not args.assume_main:   # S103 WTG: the record the main session checks before merging (tools\gate\needs_main_owed.py)
        try:
            (out_dir / ('needs_main_%s.json' % (head0 or 'unknown')[:12])).write_text(json.dumps(
                {'sha': head0, 'root': str(root), 'when': time.strftime('%Y-%m-%d %H:%M:%S'), 'rows': [r['name'] for r in nmain_rows]}, indent=1), encoding='utf-8')
        except OSError as e:
            print(f'[gate] could not write the needs-main record: {e}')
    if failed:
        if nblocked and failed == nblocked:
            return 3   # S105 E: the only non-pass rows are BLOCKED (could not run): gate_wait keeps the code, never a plain 1 = FAIL
        return 5 if nnotrun and failed == nnotrun else 1   # 5 = the only non-pass rows are NOT RUN (the --deadline): gate_wait maps it to 2 (timed out), never to a clean fail or pass
    return 4 if nmain_rows else 0   # 4 = green with needs-main (S103 WTG): ran rows pass, the listed rows are OWED, never a clean 0


if __name__ == '__main__':
    sys.exit(main())
