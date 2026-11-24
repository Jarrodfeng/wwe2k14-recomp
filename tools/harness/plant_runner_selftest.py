"""S110 PLANTFAST: selftest of tools\\harness\\plant_runner.py (the bounded parallel runner of planted-twin builds). No game runs; a few small clang++ compiles.

    py -I tools\\harness\\plant_runner_selftest.py [--jobs N]

  1. a tiny synthetic unit + header with 3 plants: one CAUGHT (the unit fails under it), one NOT caught (the unit still passes under it: judge() reports a
     problem), one whose pattern matches nothing (a problem, and never built): the verdicts are exactly those, and the run's exit code would be non-zero;
  2. the job count: default min(8, cpu // 2), env WWE_PLANT_JOBS, --jobs N, and a bad value refused;
  3. past-bug re-find: the real plant `rows_span_stops_only_at_ffffffff` (tools\\seats\\seats_selftest.py ALIAS_PLANTS, the S110 ALIASMISS fault) built through the
     pool (run_unit_plants) is caught with the SAME failing checks and return code as the same plant built and run serially, and the shipped build passes.
Exit 0 = all of it holds.
"""
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import plant_runner as pr

UNIT = r'''#include <cstdio>
#include "tiny.h"
int main() {
  int bad = 0;
  if (add(2, 3) != 5) { std::printf("FAIL add\n"); ++bad; }
  if (scale(1) != 2) { std::printf("FAIL scale\n"); ++bad; }
  std::printf(bad ? "FAIL tiny\n" : "PASS tiny\n");
  return bad ? 1 : 0;
}
'''
HEADER = 'inline int add(int a, int b) { return a + b; }\ninline int scale(int x) { return x * 2; }\ninline int never_called(int x) { return x - 7; }\n'
PLANTS = [
    ('caught_add_minus', r'return a \+ b;', 'return a - b;'),
    ('not_caught_never_called', r'return x - 7;', 'return x - 8;'),
    ('pattern_matches_nothing', r'this text is not in the header', 'x'),
]


def run_exe(exe, tag=None):
    r = subprocess.run([exe], capture_output=True, text=True, timeout=60)
    return r.returncode, r.stdout


def main():
    fails = []

    def check(name, ok, detail=''):
        print('%s %s%s' % ('PASS' if ok else 'FAIL', name, (': ' + detail) if detail and not ok else ''))
        if not ok:
            fails.append(name)

    jobs = pr.parse_jobs(sys.argv[1:])
    tmp = tempfile.mkdtemp(prefix='plant_runner_selftest_')
    try:
        # 1. the synthetic unit
        d = os.path.join(tmp, 'src')           # the shipped header; the unit lives in its own folder (a quoted include finds a sibling first)
        u = os.path.join(tmp, 'unit')
        os.makedirs(d)
        os.makedirs(u)
        open(os.path.join(u, 'tiny.cpp'), 'w').write(UNIT)
        open(os.path.join(d, 'tiny.h'), 'w', newline='').write(HEADER)
        sh, outs = pr.run_unit_plants([os.path.join(u, 'tiny.cpp')], [d], ['-std=c++17'], os.path.join(d, 'tiny.h'), [], PLANTS, run_exe, 'tiny.exe',
                                      os.path.join(tmp, 'b'), jobs)
        lines, problems = pr.judge(sh, outs, 'tiny')
        check('synthetic: the shipped unit passes', sh.error is None and sh.rc == 0 and 'PASS tiny' in sh.out)
        by = {o.name: o for o in outs}
        check('synthetic: the outcomes come back in plant order', [o.name for o in outs] == [p[0] for p in PLANTS])
        check('synthetic: the caught plant fails the unit by its own check', by['caught_add_minus'].rc == 1 and 'FAIL add' in by['caught_add_minus'].out)
        check('synthetic: the not-caught plant still passes (rc 0)', by['not_caught_never_called'].rc == 0)
        check('synthetic: the no-match plant matched 0 times and was not built', by['pattern_matches_nothing'].matches == 0 and by['pattern_matches_nothing'].art is None)
        check('synthetic: exactly two problems: not caught, no match', len(problems) == 2 and any('never_called' in p and 'still PASSES' in p for p in problems)
              and any('pattern_matches_nothing' in p and 'matched 0 times' in p for p in problems), repr(problems))
        check('synthetic: one report line for the caught plant only... and the not-caught one (both were built)', len(lines) == 2, repr(lines))
        # a plant that does not compile is a build error of that plant, not a pass
        _, o2 = pr.run_unit_plants([os.path.join(u, 'tiny.cpp')], [d], ['-std=c++17'], os.path.join(d, 'tiny.h'), [], [('broken', r'return a \+ b;', 'return a +;')], run_exe,
                                   'tiny.exe', os.path.join(tmp, 'b2'), jobs)
        check('synthetic: a plant that does not compile is an error text, not a rc', o2[0].error is not None and 'did not compile' in o2[0].error and o2[0].rc is None)

        # 2. the job count
        saved = os.environ.pop('WWE_PLANT_JOBS', None)
        try:
            check('jobs: default is min(8, cpu // 2)', pr.parse_jobs([]) == max(1, min(8, (os.cpu_count() or 2) // 2)))
            os.environ['WWE_PLANT_JOBS'] = '3'
            check('jobs: env WWE_PLANT_JOBS', pr.parse_jobs([]) == 3)
            check('jobs: --jobs beats the env', pr.parse_jobs(['--jobs', '2']) == 2 and pr.parse_jobs(['--jobs=5']) == 5)
            try:
                pr.parse_jobs(['--jobs', '0'])
                check('jobs: 0 refused', False)
            except SystemExit:
                check('jobs: 0 refused', True)
        finally:
            os.environ.pop('WWE_PLANT_JOBS', None)
            if saved is not None:
                os.environ['WWE_PLANT_JOBS'] = saved

        # 3. the day's real plant, through the pool and serially
        sys.path.append(os.path.join(REPO, 'tools', 'seats'))
        import seats_selftest as ss
        plant = [p for p in ss.ALIAS_PLANTS if p[0] == 'rows_span_stops_only_at_ffffffff']
        check('refind: the plant is still in ALIAS_PLANTS', len(plant) == 1)
        if plant:
            hdr = os.path.join(REPO, 'src', 'seat_alias_core.h')
            args = ([os.path.join(ss.HERE, 'alias_unit.cpp')], [os.path.join(REPO, 'src')], ['-std=c++20', '-Wall', '-D_CRT_SECURE_NO_WARNINGS',
                    '-D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING'], hdr, [os.path.join(REPO, 'src', s) for s in ss.SIBLINGS])
            run = lambda exe, tag: ss.run_test(exe, 'alias_unit', 'pr_' + tag)
            try:
                psh, pouts = pr.run_unit_plants(*args, plant, run, 'alias_unit.exe', os.path.join(tmp, 'pool'), jobs, fail_tail=800)
                text = open(hdr, encoding='utf-8', newline='').read()
                import re
                new, n = re.subn(plant[0][1], plant[0][2], text, count=1)
                exe, _ = pr.compile_variant(args[0], args[1], args[2], 'seat_alias_core.h', new, args[4], os.path.join(tmp, 'serial'), 'serial', 'alias_unit.exe', 800)
                src_, sout = ss.run_test(exe, 'alias_unit', 'pr_serial')
                po = pouts[0]
                fl = lambda s: [l for l in s.splitlines() if l.startswith('FAIL')]
                check('refind: the shipped alias unit passes through the pool', psh.error is None and psh.rc == 0, psh.error or psh.out[-200:])
                check('refind: the pattern matches once', n == 1 and po.matches == 1)
                check('refind: the pooled plant is caught (the unit FAILS)', po.error is None and po.rc != 0, po.error or '')
                check('refind: same return code and same failing checks as the serial build', po.rc == src_ and fl(po.out) == fl(sout) and len(fl(sout)) > 0,
                      '%r vs %r' % (fl(po.out)[:2], fl(sout)[:2]))
            finally:
                shutil.rmtree(os.path.join(REPO, 'out', 'alias_unit'), ignore_errors=True)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print('plant_runner_selftest: %s' % ('OK' if not fails else '%d failed: %s' % (len(fails), ', '.join(fails))))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
