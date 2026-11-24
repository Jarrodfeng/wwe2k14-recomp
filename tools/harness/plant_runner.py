"""S110 PLANTFAST: the shared runner of the planted-twin selftests (tools\\seats\\bind_selftest.py, seats_selftest.py, ...).

A planted-twin selftest builds its unit on the shipped header (must pass), then once per plant on a COPY of the header with one regex replacement applied
(must fail), each build in its own temp folder. The builds were one after another; they are independent compiles, so this runs them on a bounded pool.
Nothing is weakened: every plant is still compiled from its own planted source, run, and judged by the caller exactly as before (the caller keeps its own
verdict rules: what counts as caught, and how a compile error counts). The helper only does the mechanics and hands back each outcome IN PLANT ORDER.

    import plant_runner as pr                                  # sys.path has tools\\harness
    jobs = pr.parse_jobs(sys.argv[1:])                         # --jobs N, else env WWE_PLANT_JOBS, else min(8, cpu_count // 2)
    sh, outs = pr.run_plants(text, PLANTS, build, run, jobs)   # build(header_text, tag) -> artefact (raises RuntimeError when it does not compile)
                                                               # run(artefact, tag) -> (returncode, stdout)
    sh.rc, sh.out, sh.art, sh.error                            # the shipped build: error = the RuntimeError text, else None
    for o in outs: o.name, o.matches (re.subn count), o.error (build error), o.rc, o.out, o.art

A plant whose pattern does not match exactly once has matches != 1 and is NOT built (the caller reports it, as the scripts always did).

Bound: the pool is opened under the compile queue's hold (tools\\admit\\admit_py.py, kind cc.tool: what admit grants is the most that run at once; with no admit
installed the fall-open rule gives the wanted count) and never exceeds the plant count + 1. Real compile work only: no polling, no sleeps.
"""
import os
import re
import sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.append(os.path.join(os.path.dirname(HERE), 'admit'))
import admit_py as _admit   # the machine-wide compile queue (nothing runs at import)
import plant_runner_once as _once   # S110 PLANTWIRE: compile once, pick the plant at run time (env PLANT_ONCE=0 = every plant is built alone)
from plant_runner_once import Once, Plant, fail_lines, make_spec, once_line   # noqa: F401  (the scripts' side of the join)


class Outcome:
    def __init__(self, name):
        self.name, self.matches, self.error, self.rc, self.out, self.art = name, 1, None, None, '', None


def default_jobs():
    return max(1, min(8, (os.cpu_count() or 2) // 2))


def parse_jobs(argv):
    """--jobs N / --jobs=N from argv (a bad value is a SystemExit 2), else env WWE_PLANT_JOBS, else the default."""
    n = None
    for i, a in enumerate(argv):
        if a == '--jobs' and i + 1 < len(argv):
            n = argv[i + 1]
        elif a.startswith('--jobs='):
            n = a[7:]
    if n is None:
        n = os.environ.get('WWE_PLANT_JOBS')
    if n in (None, ''):
        return default_jobs()
    try:
        v = int(n)
    except ValueError:
        raise SystemExit('plant_runner: --jobs / WWE_PLANT_JOBS must be a number, got %r' % n)
    if v < 1:
        raise SystemExit('plant_runner: --jobs / WWE_PLANT_JOBS must be >= 1, got %d' % v)
    return v


def find_clang():
    import shutil
    for c in (shutil.which('clang++'), r'C:\Program Files\LLVM\bin\clang++.exe', '/usr/bin/clang++'):
        if c and os.path.isfile(c):
            return c
    raise RuntimeError('no clang++ found (tools\\env.ps1 puts the project\'s LLVM on PATH)')


def compile_variant(sources, include_dirs, flags, header_name, header_text, sibling_paths, tmp, tag, exe_name, fail_tail=1200, timeout=300):
    """THE per-variant step (a compile-once path can be put in front of it; this stays the per-plant fallback): builds one variant of a unit in its OWN folder
    tmp\\tag: the sibling headers (paths) are copied there, header_text is written there as header_name, and the unit's sources are compiled with
    -I that folder FIRST (the planted copy wins), then include_dirs. -> (exe path, compiler output); RuntimeError '<exe_name> did not compile (<tag>): <tail>'
    when it does not compile."""
    import shutil
    import subprocess
    inc = os.path.join(tmp, tag)
    os.makedirs(inc, exist_ok=True)
    for sib in sibling_paths:
        if os.path.basename(sib) != header_name:
            shutil.copy(sib, os.path.join(inc, os.path.basename(sib)))
    with open(os.path.join(inc, header_name), 'w', encoding='utf-8', newline='') as f:
        f.write(header_text)
    exe = os.path.join(inc, exe_name)
    cmd = [find_clang()] + list(flags) + ['-I', inc]
    for d in include_dirs:
        cmd += ['-I', d]
    r = subprocess.run(cmd + list(sources) + ['-o', exe], capture_output=True, text=True, timeout=timeout)
    if r.returncode != 0 or not os.path.isfile(exe):
        raise RuntimeError('%s did not compile (%s): %s' % (os.path.splitext(exe_name)[0], tag, (r.stdout + r.stderr)[-fail_tail:]))
    return exe, (r.stdout + r.stderr)


def run_unit_plants(sources, include_dirs, flags, header_path, sibling_paths, plants, run, exe_name, tmp, jobs=None, fail_tail=1200, timeout=300,
                    caught=None, run_args=None):
    """The whole contract in one call: (unit sources, include dirs, flags, the header path + sibling header paths, plants, jobs) -> (shipped, [per plant]).
    run(exe, tag) -> (returncode, stdout). Each Outcome.art is (exe, compiler output). Builds on compile_variant; run_plants is the pool underneath."""
    header_name = os.path.basename(header_path)
    text = open(header_path, encoding='utf-8', newline='').read()
    once = None
    if caught is not None and len(sources) == 1:   # caught(rc, out): what the script reports with no problem; run_args(tag): the unit's argv
        once = Once(make_spec(sources[0], {header_name: header_path}, flags, include_dirs, {os.path.basename(s): s for s in sibling_paths}, run_args), caught, header_name)
    return run_plants(text, plants, lambda t, tag: compile_variant(sources, include_dirs, flags, header_name, t, sibling_paths, tmp, tag, exe_name, fail_tail, timeout),
                      lambda art, tag: run(art[0], tag), jobs, once)


def judge(sh, outs, unit_name):
    """The strict verdict (bind_selftest's): -> (report lines, problems). A pattern that matches not exactly once, a plant that does not compile, a unit that still
    PASSES, or one that ends without a FAIL line and its summary (a crash) is a problem; the shipped build must pass. Scripts with their own rules keep them."""
    lines, problems = [], []
    if sh.error:
        problems.append(sh.error)
    elif sh.rc != 0 or ('PASS ' + unit_name) not in sh.out:
        problems.append('the unit FAILS on the shipped header (or ended without its summary line)')
    for o in outs:
        if o.matches != 1:
            problems.append('plant %s: the pattern matched %d times (the header changed under the plant)' % (o.name, o.matches))
        elif o.error:
            problems.append('plant %s: %s' % (o.name, o.error))
        else:
            failed = [l for l in o.out.splitlines() if l.startswith('FAIL ')]
            if o.rc == 0:
                problems.append('plant %s: the unit still PASSES' % o.name)
            elif not failed or ('FAIL ' + unit_name) not in o.out:
                problems.append('plant %s: the unit ended with code %d without a FAIL line and its summary (a crash, not a caught fault)' % (o.name, o.rc))
            lines.append('plant %-40s unit %s (%d failing checks)' % (o.name, 'FAILS' if o.rc else 'PASSES', len(failed)))
    return lines, problems


def _one(o, text, build, run):
    try:
        o.art = build(text, o.name)
        o.rc, o.out = run(o.art, o.name)
    except RuntimeError as e:
        o.error = str(e)
    return o


def run_plants(text, plants, build, run, jobs=None, once=None):
    """-> (shipped Outcome, [Outcome per plant, in order]). See the module text."""
    variants = []
    for name, pat, rep in plants:
        new, n = re.subn(pat, rep, text, count=1)
        variants.append((name, n, new))
    if once is not None and once.plants is None:
        once = Once(once.spec, once.caught, once.header, [Plant(n, once.header, p, r) for n, p, r in plants])
    return run_variants(text, variants, build, run, jobs, once)


def run_variants(shipped_input, variants, build, run, jobs=None, once=None):
    """The pool under run_plants, for a caller that applies its own plants (a plant of one of several headers, a literal replacement): variants =
    [(name, matches, planted_input)]; a variant with matches != 1 is not built. build / run get the input (any object the caller's build understands)."""
    jobs = jobs or default_jobs()
    sh = Outcome('shipped')
    text = shipped_input
    outs, todo = [], []
    for name, n, new in variants:
        o = Outcome(name)
        o.matches = n
        outs.append(o)
        if n == 1:
            todo.append((o, new))
    sh_done = False
    if once is not None:   # S110 PLANTWIRE: the compile-once path first; it leaves the plants it could not settle (and the shipped one when it fell through)
        sh_done, todo = _once.try_once(once, sh, todo, build, run, jobs, _one)
        if sh_done and not todo:
            return sh, outs
    want = max(1, min(jobs, len(todo) + 1))
    with _admit.hold('cc.tool', want, timeout_s=300) as nj, ThreadPoolExecutor(max_workers=max(1, min(int(nj), want))) as ex:   # the compile queue's count, never more than the want
        futs = ([] if sh_done else [ex.submit(_one, sh, text, build, run)]) + [ex.submit(_one, o, new, build, run) for o, new in todo]
        for f in futs:
            f.result()
    return sh, outs
