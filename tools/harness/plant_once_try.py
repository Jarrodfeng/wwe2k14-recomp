"""S110 PLANTONCE: runs plant_once.run_once on a selftest script's OWN plant lists and unit definitions (imported, not copied). No game runs.

    py -I tools/harness/plant_once_try.py bind      tools/seats/bind_selftest.py (133 plants)
    py -I tools/harness/plant_once_try.py seats     tools/seats/seats_selftest.py (5 lists; the three alias_unit lists share one exe)

Per plant: `name: run-time|recompile: caught|NOT CAUGHT`, then `plant-once: N at run time, M by recompile (<names>)`, then wall seconds and
compile counts. Exit 1 on any NOT CAUGHT / no-match / shipped failure. The scripts are not edited; `recompile` does what each script does for one plant.
"""
import os
import re
import shutil
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
SEATS = os.path.join(os.path.dirname(HERE), 'seats')
sys.path.insert(0, SEATS)
import plant_once as po  # noqa: E402


def bind_jobs():
    import bind_selftest as bs
    text = open(bs.HEADER, encoding='utf-8', newline='').read()
    defs = ['-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-D_CRT_SECURE_NO_WARNINGS', '-D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING']
    spec = po.Spec(
        unit_cpp=os.path.join(bs.HERE, 'bind_unit.cpp'), headers={bs.NAME: bs.HEADER},
        siblings={'seats_core.h': os.path.join(bs.REPO, 'src', 'seats_core.h')},
        includes=[os.path.join(bs.REPO, 'src'), os.path.join(bs.REPO, 'src', 'launcher')],
        flags=['-std=c++20', '-Wall', '-Wno-unused-command-line-argument'] + defs,
        plants=[po.Plant(n, bs.NAME, p, r) for n, p, r in bs.PLANTS])

    def verdict(rc, out):
        return rc != 0 and any(l.startswith('FAIL ') for l in out.splitlines()) and 'FAIL bind_unit' in out

    def recompile_for(tmp, count):
        def recompile(p):
            count.append(p.name)
            new, n = re.subn(p.pat, p.rep, text, count=1)
            try:
                pexe, _ = bs.build(new, tmp, p.name)
            except RuntimeError as e:
                return False, str(e)[:200]
            prc, pout = bs.run_test(pexe)
            return verdict(prc, pout), ('the unit still PASSES' if prc == 0 else (prc, pout))
        return recompile
    return [(spec, verdict, recompile_for, None)]


def seats_jobs():
    import seats_selftest as ss
    lists = [('seats_unit', 'seats_core.h', ss.PLANTS), ('alias_unit', 'seat_alias_core.h', ss.ALIAS_PLANTS),
             ('alias_unit', 'seat_card.h', ss.CARD_PLANTS), ('alias_unit', 'seat_bind_core.h', ss.REFFORM_PLANTS),
             ('alias_plan_unit', 'seat_alias_plan.h', ss.PLAN_PLANTS)]
    by_unit = {}
    for unit, hdr, plants in lists:
        by_unit.setdefault(unit, []).append((hdr, plants))
    defs = ['-D_CRT_SECURE_NO_WARNINGS', '-D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING']

    def verdict(rc, out):
        return rc != 0

    jobs = []
    for unit, hl in by_unit.items():
        headers = {h: os.path.join(ss.REPO, 'src', h) for h, _ in hl}
        plants = [po.Plant(n, h, p, r) for h, pl in hl for n, p, r in pl]
        spec = po.Spec(unit_cpp=os.path.join(ss.HERE, unit + '.cpp'), headers=headers, flags=['-std=c++20', '-Wall'] + defs,
                       siblings={s: os.path.join(ss.REPO, 'src', s) for s in ss.SIBLINGS},
                       includes=[os.path.join(ss.REPO, 'src')], plants=plants,
                       run_args=lambda tag, u=unit: [os.path.join(ss.REPO, 'out', u, tag)])

        def recompile_for(tmp, count, unit=unit, hl=hl):
            def recompile(p):
                count.append(p.name)
                text = open(os.path.join(ss.REPO, 'src', p.header), encoding='utf-8', newline='').read()
                new, n = re.subn(p.pat, p.rep, text, count=1)
                try:
                    exe = ss.build(unit, p.header, new, tmp, p.name)
                except RuntimeError as e:
                    return False, str(e)[:200]
                prc, pout = ss.run_test(exe, unit, p.name)
                return verdict(prc, pout), ('the unit still PASSES' if prc == 0 else (prc, pout))
            return recompile
        jobs.append((spec, verdict, recompile_for, os.path.join(ss.REPO, 'out', unit)))
    return jobs


def main():
    which = sys.argv[1] if len(sys.argv) > 1 else ''
    if which not in ('bind', 'seats'):
        print(__doc__)
        return 2
    t0 = time.time()
    bad, run_time, fall_back, compiles, recompiles, total = [], 0, [], 0, 0, 0
    for spec, verdict, recompile_for, outdir in (bind_jobs() if which == 'bind' else seats_jobs()):
        tmp = tempfile.mkdtemp(prefix='plant_once_try_')
        spec.tmp = os.path.join(tmp, 'once')
        spec.log = lambda s: print(s, flush=True)
        count = []
        try:
            results, fallback = po.run_once(spec, verdict, recompile_for(os.path.join(tmp, 'rc'), count))
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
            if outdir:
                shutil.rmtree(outdir, ignore_errors=True)
        st = spec.stats
        if not st['shipped_pass']:
            bad.append('the unit does not PASS on the shipped headers')
        if st['shipped_warn']:
            bad.append('the shipped unit compiles with warnings')
        if not st['id0_pass'] and not st['abandoned'] and any(r[0] == 'run-time' for r in results.values()):
            bad.append('id 0 of the combined exe does not PASS')
        compiles += st['compiles']
        recompiles += len(count)
        for name, (how, caught, detail) in results.items():
            total += 1
            print('%s: %s: %s' % (name, how, 'caught' if caught else 'NOT CAUGHT'))
            if not caught:
                bad.append('%s: %s' % (name, detail if how == 'no-match' else 'not caught (%s)' % (detail if isinstance(detail, str) else 'ran, verdict false')))
        run_time += sum(1 for r in results.values() if r[0] == 'run-time')
        fall_back += fallback
        print('  demoted by the trial compile: %s; rounds %d; abandoned %s' % (', '.join(st['demoted']) or '-', st['rounds'], st['abandoned']))
    print('plant-once: %d at run time, %d by recompile (%s)' % (run_time, len(fall_back), ', '.join(fall_back)))
    print('plants %d; compiles: %d for the combined path + %d recompiles; wall %.1f s' % (total, compiles, recompiles, time.time() - t0))
    for b in bad:
        print('PROBLEM: ' + b)
    print('plant_once_try %s: %s' % (which, 'OK' if not bad else '%d problem(s)' % len(bad)))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
