"""S110 PLANTWIRE: the join of plant_runner (the scripts' entry) and plant_once (compile the unit once, pick the planted fault at run time).

plant_runner.run_variants calls try_once() FIRST when the caller gave it a `Once` (unless env PLANT_ONCE=0). try_once hands plant_once.run_once every
plant whose pattern matched; the runner's own per-plant build+run (`one`) is the `recompile` callback, so it runs only for the plants plant_once demotes
(constants outside a function body, consteval, warns, a crash or timeout at run time) and for the ones the combined exe did not catch. Whatever the
combined exe caught is reported with that run's own (rc, output); the script's verdict rules then read the Outcomes exactly as on the recompile path.

    Once(spec, caught, header='', plants=None)
      spec    make_spec(...): the unit's .cpp, headers {name: path} that carry plants, flags (with the script's warning flags), includes, siblings, run_args
      caught  (rc, out) -> bool: True ONLY when the script would report the plant with no problem (a stricter rule is safe: the plant is then rebuilt alone)
      header  run_plants: the one header its (name, pattern, replacement) plants are on;  plants: run_variants callers give [plant_once.Plant] themselves

The runs are counted; once_line() prints `plant-once: N at run time, M by recompile (<names>)` ONCE (the scripts call it before their summary) and
prints nothing when nothing was tried (PLANT_ONCE=0).
"""
import dataclasses
import os
import shutil
import sys
import tempfile
from dataclasses import dataclass

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.append(HERE)
sys.path.append(os.path.join(os.path.dirname(HERE), 'admit'))
import admit_py as _admit   # noqa: E402
import plant_once as po     # noqa: E402

Plant = po.Plant
_LOG = []   # [(plants run at run time, [names recompiled])] of every try_once of this process


@dataclass
class Once:
    spec: object
    caught: object
    header: str = ''
    plants: list = None


def enabled():
    return os.environ.get('PLANT_ONCE', '') != '0'


def fail_lines(summary=None, prefix='FAIL '):
    """caught(rc, out) of the strict scripts: a failing exit, a printed `prefix` line and (summary given) the unit's own `FAIL <summary>` line."""
    return lambda rc, out: rc != 0 and any(l.startswith(prefix) for l in out.splitlines()) and (summary is None or ('FAIL ' + summary) in out)


def make_spec(unit_cpp, headers, flags, includes=(), siblings=None, run_args=None):
    return po.Spec(unit_cpp=unit_cpp, headers=dict(headers), plants=[], flags=list(flags), siblings=dict(siblings or {}), includes=list(includes), run_args=run_args)


def try_once(once, sh, todo, build, run, jobs, one):
    """-> (shipped done, todo still to build the old way). todo = [(Outcome, planted input)] of the plants whose pattern matched."""
    names = {o.name: (o, new) for o, new in todo}
    plants = [p for p in (once.plants or []) if p.name in names]
    if not enabled() or not todo or len(plants) != len(todo):
        return False, todo
    want = max(1, min(jobs, len(todo) + 1))
    tmp = tempfile.mkdtemp(prefix='plant_runner_once_')
    try:
        with _admit.hold('cc.tool', want, timeout_s=300) as nj:
            spec = dataclasses.replace(once.spec, plants=plants, tmp=os.path.join(tmp, 'once'), jobs=max(1, min(int(nj), want)), log=None, stats={})

            def recompile(p):
                o, new = names[p.name]
                one(o, new, build, run)
                return (o.error is None and bool(once.caught(o.rc, o.out))), (o.error or 'ran')
            try:
                results, fallback = po.run_once(spec, once.caught, recompile)
            except (RuntimeError, ValueError, OSError) as e:
                for o, _ in todo:
                    o.error, o.rc, o.out, o.art = None, None, '', None
                _LOG.append((0, ['all: ' + str(e).strip().splitlines()[0][:100] if str(e).strip() else 'all']))
                return False, todo
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    st = spec.stats
    sh.rc, sh.out, sh.art = st['shipped_rc'], st['shipped_out'], (None, st['shipped_warn'])
    left, at_run_time = [], 0
    for name, (how, _caught, detail) in results.items():
        o, new = names[name]
        if how == 'run-time':
            o.rc, o.out = detail
            at_run_time += 1
        elif how == 'no-match':
            left.append((o, new))
    _LOG.append((at_run_time, list(fallback)))
    return True, left


def once_line():
    """Print the one `plant-once:` line for every try_once so far (none: nothing printed) and forget them."""
    if not _LOG:
        return
    n = sum(a for a, _ in _LOG)
    names = [x for _, b in _LOG for x in b]
    print('plant-once: %d at run time, %d by recompile (%s)' % (n, len(names), ', '.join(names)))
    del _LOG[:]
