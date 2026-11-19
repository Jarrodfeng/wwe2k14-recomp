#!/usr/bin/env python3
# S102 lane M5: offline proof of the multi-arm ab job (bench.ps1 -ArmFlags / -AbPlan, s0_5_jobs.ps1 -CheckOnly, ab_line.py's multi-arm reader).
# 0 game runs, 0 Surface runs. Run: py -I tools\surface\ab_multi_check.py [--root .]
#   1. every .ps1 touched parses (PowerShell's own parser).
#   2. the plan: 3 arms x 2 rounds, 4 arms x 3 rounds, 5 arms x 3 rounds (and a plain A:|B: pair = the old A B / B A order): folders named ab_<round>_<arm>
#      (a round's second base run ab_<round>_A2), every B arm once per round with an A beside it, the arm that runs last differs between rounds.
#   3. planted twins: an order where one arm always runs last, a B arm with no A beside it, an arm twice in a round: each REFUSED (exit 1, named reason);
#      the unplanted twin of each passes (exit 0).
#   4. the job line: a good ArmFlags line passes s0_5_jobs.ps1 -CheckOnly; five malformed ones are refused.
#   5. ab_line.py: the real two-arm folders print exactly as the saved reference lines of this check (the old tool's output is re-derived from the
#      same folders through the two-arm path = a diff of "multi path never taken"); a synthetic multi-arm tree (real frames csvs copied under new names)
#      is paired with the nearest A in its round and the numbers equal the real arms' own lines; a planted +3 W on one pair leaves that pair out; +3 W
#      on every pair of an arm = POWER-CONFOUNDED.
# Needs out\s102\ab8\g2banks\x (the real arms) in the MAIN checkout; SKIPs the real-data part loudly (exit 3) when it is missing.
import argparse, os, re, shutil, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MAIN = Path(r'C:\Users\Jarrod\wwe2k14-recomp')
FAILS = []


def check(ok, what):
    print(('PASS  ' if ok else 'FAIL  ') + what)
    if not ok:
        FAILS.append(what)


def pwsh(*args):
    r = subprocess.run(['pwsh', '-NoProfile', '-File', *map(str, args)], capture_output=True, text=True, cwd=ROOT)
    return r.returncode, r.stdout + r.stderr


def plan(arms, rounds, warm=True, order=None):
    a = [str(ROOT / 'tools/surface/bench.ps1'), '-AbPlan', '-AbRounds', str(rounds)] + (['-AbWarmup'] if warm else [])
    a += ['-AbPlanOrder', order] if order else ['-ArmFlags', arms]
    return pwsh(*a)


def rounds_of(out):
    rs = []
    for m in re.finditer(r'^round (\d+): (.*)$', out, re.M):
        rs.append([re.sub(r'^A\d+$', 'A', re.match(r'ab_\d+_(.+)$', f).group(1)) for f in m.group(2).split()])
    return rs


def real_lines(folder, split=False):
    r = subprocess.run([sys.executable, '-I', str(ROOT / 'tools/surface/ab_line.py'), str(folder)] + (['--split'] if split else []), capture_output=True, text=True)
    return r.returncode, r.stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=None)
    a = ap.parse_args()
    global ROOT
    if a.root:
        ROOT = Path(a.root).resolve()

    # 1. parse check
    for f in ('tools/surface/bench.ps1', 'tools/surface/s0_5_jobs.ps1'):
        code = "$e=$null;[void][System.Management.Automation.Language.Parser]::ParseFile('%s',[ref]$null,[ref]$e);if($e){$e|%%{$_.ToString()};exit 1}" % str(ROOT / f).replace("'", "''")
        r = subprocess.run(['pwsh', '-NoProfile', '-Command', code], capture_output=True, text=True)
        check(r.returncode == 0, 'parser: %s has no parse errors %s' % (f, r.stdout.strip()[:200]))

    # 2. plans
    cases = [('A:x|g2:y|g3:z', 2), ('A:x|g2:y|g3:z|g5:w', 3), ('A:x|g2:y|g3:z|g5:w|g6:q', 3), ('A:|g2:y', 3)]
    for arms, nr in cases:
        rc, out = plan(arms, nr)
        rs = rounds_of(out)
        names = [x.split(':')[0] for x in arms.split('|')]
        bs = [n for n in names if n != 'A']
        ok = rc == 0 and len(rs) == nr and 'plan ok' in out
        ok = ok and all(sorted(x for x in rd if x != 'A') == sorted(bs) for rd in rs)
        ok = ok and all(any(rd[i - 1:i] == ['A'] or rd[i + 1:i + 2] == ['A'] for _ in [0]) for rd in rs for i, x in enumerate(rd) if x != 'A')
        ok = ok and (nr < 2 or len({rd[-1] for rd in rs}) > 1)
        k = len(bs)
        want_runs = 1 + nr * (k + (k + 1) // 2)
        m = re.search(r'(\d+) runs', out)
        ok = ok and m and int(m.group(1)) == want_runs
        check(ok, 'plan %d arms x %d rounds: every B once per round, an A beside each, last arm rotates, %d runs (%s)' % (len(names), nr, want_runs, out.strip().splitlines()[-2:] if not ok else 'ok'))
        for line in out.splitlines()[:nr + 1]:
            print('       ' + line)
    rc, out = plan('A:|B:y', 2)
    check(rc == 0 and rounds_of(out) == [['A', 'B'], ['B', 'A']], 'plan A + B: the old A B / B A order (%s)' % rounds_of(out))
    rc, out = plan('A:x|g2:y|g3:z', 2, warm=False)
    check('abwarm' not in out, 'plan without -AbWarmup has no warm-up run')

    # 3. planted twins
    twins = [('always last', 'g2,A,g3;g2,A,g3', 'runs last in every round', 'g2,A,g3;g3,A,g2'),
             ('no A beside', 'A,g2,g3,g5;g5,A,g2,g3', 'has no A beside it', 'g2,A,g3,g5,A;g5,A,g2,A,g3'),
             ('arm twice', 'g2,A,g3;g3,A,g2,g2', 'runs 2 times', 'g2,A,g3;g3,A,g2')]
    for name, bad, reason, good in twins:
        rc, out = plan(None, 2, order=bad)
        check(rc == 1 and reason in out, 'planted twin "%s" is REFUSED with its reason (rc %d)' % (name, rc))
        rc, out = plan(None, 2, order=good)
        check(rc == 0 and 'plan ok' in out, 'unplanted twin of "%s" passes (rc %d)' % (name, rc))
    rc, out = plan('g2:x|g3:y', 2)
    check(rc != 0 and 'arm named A' in out, 'arms without an A are refused')
    rc, out = plan('A:x|A2:y', 2)
    check(rc != 0, 'an arm named A2 (the base repeat name) is refused')

    # 4. job line
    good = 'g|Only=ab;Base=none;AbScene=frames_bryan;AbRounds=3;AbWarmup=true;ArmFlags=A:a=1,b=2|g2:c=3|g3:d=4'
    bad = ['g|Only=ab;Base=none;ArmFlags=g2:z|g3:w', 'g|Only=ab;Base=none;ArmFlags=A:x|A:y', 'g|Only=frames_bryan;Base=none;ArmFlags=A:x|g2:y', 'g|Only=ab;Base=none;ArmFlags=A:x|g2:y;ArmBFlags=q', 'g|Only=ab;Base=none;ArmFlags=A:x']
    tmp = Path(tempfile.mkdtemp(prefix='m5_jobs_'))
    rc, out = pwsh(ROOT / 'tools/surface/s0_5_jobs.ps1', '-Out', tmp / 'o', '-CheckOnly', '-Jobs', good)
    check(rc == 0 and 'job lines ok' in out, 'job line with ArmFlags passes the runner validation')
    for j in bad:
        rc, out = pwsh(ROOT / 'tools/surface/s0_5_jobs.ps1', '-Out', tmp / 'o', '-CheckOnly', '-Jobs', j)
        check(rc != 0 and 'bad job line' in out, 'malformed job line refused: %s' % j[2:60])
    check(not (tmp / 'o').exists(), 'the validation touched no output folder (and no lock)')
    shutil.rmtree(tmp, ignore_errors=True)

    # 5. ab_line.py
    src = MAIN / 'out/s102/ab8/g2banks/x'
    if not (src / 'ab_1_A').is_dir():
        print('SKIP  real data %s missing: part 5 not run' % src)
        return 3 if not FAILS else 1
    rc, ref = real_lines(src)
    check(rc == 0 and 'pair 1: B - A' in ref and 'arm ' not in ref, 'two-arm folder still takes the two-arm path (no "arm" summary table)')
    shown = {}
    for m in re.finditer(r'^(ab_\d_[AB])\s+shown\s+([\d.]+)\s+1% low\s+([\d.]+)\s+gpu\s+([\d.]+)\s+render\s+([\d.]+)\s+swap\s+([\d.]+).*pkg ([\d.]+) W', ref, re.M):
        shown[m.group(1)] = (float(m.group(2)), float(m.group(7)))
    check(len(shown) == 6, 'read the six real arms from the two-arm output')

    def build(root, layout, order, bump=None):
        """layout: {folder name: real arm}; order: folder names in run order; bump: {folder name: +W}"""
        x = root / 'x'
        for nm, real in layout.items():
            shutil.copytree(src / real, x / nm)
        (x / 'ab_order.txt').write_text(''.join('%s\t%s\t00:00:%02d\n' % (n, re.sub(r'ab_\d+_', '', n), i) for i, n in enumerate(order)))
        for nm, w in (bump or {}).items():
            pf = x / nm / 'power.csv'
            lines = pf.read_text().splitlines()
            hdr = lines[0].split(',')
            ix = hdr.index('rapl_pkg')
            out = [lines[0]]
            for ln in lines[1:]:
                c = ln.split(',')
                c[ix] = '%.2f' % (float(c[ix]) + w * 1000)
                out.append(','.join(c))
            st = pf.stat()
            pf.write_text('\n'.join(out) + '\n')
            os.utime(pf, (st.st_atime, st.st_mtime))   # bench_line refuses a power.csv newer than the log (stale check)
        return x

    # round 1: g2 A g3 g5 A2 g6   round 2: g6 A g2 g3 A2 g5 (the plan's shape for 4 B arms); values from the real six arms
    layout = {'ab_1_g2': 'ab_1_B', 'ab_1_A': 'ab_1_A', 'ab_1_g3': 'ab_2_B', 'ab_1_g5': 'ab_3_B', 'ab_1_A2': 'ab_2_A', 'ab_1_g6': 'ab_1_B',
              'ab_2_g6': 'ab_2_B', 'ab_2_A': 'ab_3_A', 'ab_2_g2': 'ab_3_B', 'ab_2_g3': 'ab_1_B', 'ab_2_A2': 'ab_1_A', 'ab_2_g5': 'ab_2_B'}
    order = ['ab_1_g2', 'ab_1_A', 'ab_1_g3', 'ab_1_g5', 'ab_1_A2', 'ab_1_g6', 'ab_2_g6', 'ab_2_A', 'ab_2_g2', 'ab_2_g3', 'ab_2_A2', 'ab_2_g5']
    t = Path(tempfile.mkdtemp(prefix='m5_syn_'))
    try:
        x = build(t / 'a', layout, order)
        rc, out = real_lines(x)
        check(rc == 0 and '=== summary' in out, 'multi-arm folder reads, with a summary table')
        # expected: nearest A in the round. g3 r1 (pos 2): A pos 1 (d1) vs A2 pos 4 (d2) -> A; g5 r1 (pos 3): A2 (d1); g6 r1 (pos 5): A2 (d1); g2 r1: A (d1)
        # round 2: g6 (pos 0) -> A (pos 1); g2 (pos 2) -> A; g3 (pos 3) -> A2 (pos 4); g5 (pos 5) -> A2.
        exp = {('g2', 1): ('ab_1_B', 'ab_1_A'), ('g3', 1): ('ab_2_B', 'ab_1_A'), ('g5', 1): ('ab_3_B', 'ab_2_A'), ('g6', 1): ('ab_1_B', 'ab_2_A'),
               ('g6', 2): ('ab_2_B', 'ab_3_A'), ('g2', 2): ('ab_3_B', 'ab_3_A'), ('g3', 2): ('ab_1_B', 'ab_1_A'), ('g5', 2): ('ab_2_B', 'ab_1_A')}
        allok = True
        for (nm, rd), (rb, ra) in exp.items():
            m = re.search(r'^pair r%d %s: B - A shown\s*([+-][\d.]+)\s+swap.*pkg A ([\d.]+) W, B ([\d.]+) W -> (.*)$' % (rd, nm), out, re.M)
            want = round(shown[rb][0] - shown[ra][0], 1)
            if not m or abs(float(m.group(1)) - want) > 0.051 or abs(float(m.group(2)) - shown[ra][1]) > 0.051 or abs(float(m.group(3)) - shown[rb][1]) > 0.051:
                allok = False
                print('       pair %s r%d: got %s want shown %+.1f, pkg A %.1f B %.1f' % (nm, rd, m.groups() if m else None, want, shown[ra][1], shown[rb][1]))
        check(allok, 'every pair is B - its nearest A in the same round (8 pairs, values equal the real arms\' own lines)')
        check(len(re.findall(r'^pair r\d g\d', out, re.M)) == 8, 'exactly one pair row per B run')
        # twin: 3 W on one pair
        # g3 round 2 is ab_2_g3; +3 W on it leaves that pair out
        x2 = build(t / 'b', layout, order, bump={'ab_2_g3': 3.0})
        rc, out2 = real_lines(x2)
        check(re.search(r'pair r2 g3:.*POWER STEP \(left out\)', out2) and re.search(r'power-matched B - A \(1 of 2 pairs\)', out2.split('--- arm g5')[0].split('--- arm g3')[1]),
              'planted +3 W on one pair: that pair is left out (g3: 1 of 2 matched)')
        check(not re.search(r'POWER STEP', out), 'the unplanted twin has no power step')
        # a +1.0 W change stays inside the 1.5 W rule when the original gap is small: use g5 r1 (A2 vs g5)
        # both pairs of g6 shifted: confounded
        x3 = build(t / 'c', layout, order, bump={'ab_1_g6': 3.0, 'ab_2_g6': 3.0})
        rc, out3 = real_lines(x3)
        check('POWER-CONFOUNDED: 0 of 2 pairs matched' in out3 and re.search(r'^g6\s+0/2\s+-.*CONFOUNDED', out3, re.M), 'planted +3 W on every g6 run: arm g6 is POWER-CONFOUNDED, the other arms still read')
        # split path
        rc, out4 = real_lines(x, split=True)
        check(rc == 0 and '=== summary' in out4, 'multi-arm with --split runs (real csvs have no split columns: "not measured" is printed, no crash)')
    finally:
        shutil.rmtree(t, ignore_errors=True)
    print('%d failed' % len(FAILS))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
