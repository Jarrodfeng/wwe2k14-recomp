#!/usr/bin/env python3
# S102 lane M6: offline proof of the bench step frames_bryan_heavy (tools\input_scripts\scenes\bryan_heavy.txt).
# 0 game runs, 0 Surface runs.  Run: py -I tools\surface\m6_window_check.py [--root .]
#   1. the route parses: every command is one the game's parser knows (read from src\input_script.h), the argument shapes of the commands the route uses
#      are right, `include` resolves, the marks come in the order tron_324 < heavy < end at tron + 760 / + 1330, and its launch flags equal the
#      frames_bryan scene step's. Planted twins: a malformed number, an unknown command, a missing end mark: each FAILs with its reason.
#   2. the wiring: bench.ps1's $sceneSteps has frames_bryan_heavy (route + the frames_bryan flags), push_kit.ps1 ships the route, bench.ps1 / push_kit.ps1
#      parse (PowerShell's parser), s0_5_jobs.ps1 -CheckOnly accepts an AbScene=frames_bryan_heavy job line, ab_line.py accepts --scene frames_bryan_heavy.
#   3. the window, from the 30 real Surface runs of out\s102\ab8 (main checkout; SKIP exit 3 when absent): the slow frames (present gap >= 33.3 ms) are
#      clustered, the heavy window holds >= 70 % of them (the old step15 window < 35 %), its p99 is at or above the whole entrance's, the early
#      window tron+20..+620 FAILs the same assertion (planted twin).
#   4. bench_line.py on a copy of a real run whose log marks are rewritten to the heavy route's: window frames T+760..T+1330, numbers equal an
#      independent re-derivation from the csv; without scene=end or without scene=heavy it REFUSES; ab_line.py reads it too.
#   5. the estimate: slot and 19-run job length from the logs' own times.
import argparse, csv, re, shutil, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MAIN = Path(r'C:\Users\Jarrod\wwe2k14-recomp')
AB8 = MAIN / 'out' / 's102' / 'ab8'
FAILS = []
HEAVY0, HEAVYN = 760, 570
SLOW_MS = 33.3


def check(ok, what):
    print(('PASS  ' if ok else 'FAIL  ') + what)
    if not ok:
        FAILS.append(what)


def known_commands(root):
    return set(re.findall(r'cmd == "(\w+)"', (root / 'src' / 'input_script.h').read_text(encoding='utf-8', errors='replace')))


def lint_route(text, known, nav_dir):
    """-> (problems, marks) ; marks = [(name, frames after the tron mark)] following the route's wait_frames."""
    probs, marks, t, seen_tron = [], [], 0, False
    for n, raw in enumerate(text.splitlines(), 1):
        line = raw.split('#', 1)[0].strip() if raw.lstrip().startswith('#') else raw.strip()
        if not line or line.startswith('#'):
            continue
        w = line.split()
        c = w[0]
        if c not in known:
            probs.append(f'line {n}: unknown command {c!r}')
            continue
        if c == 'wait_frames':
            if len(w) != 2 or not w[1].isdigit() or int(w[1]) <= 0:
                probs.append(f'line {n}: wait_frames wants one positive integer, got {w[1:]}')
            elif seen_tron:
                t += int(w[1])
        elif c == 'wait_file':
            if len(w) < 3 or not w[2].isdigit():
                probs.append(f'line {n}: wait_file wants NAME TIMEOUT, got {w[1:]}')
        elif c == 'mark':
            if len(w) < 3 or not w[1].isdigit():
                probs.append(f'line {n}: mark wants N NAME, got {w[1:]}')
            else:
                if w[2] == 'tron_324':
                    seen_tron = True
                marks.append((w[2], t))
        elif c == 'include':
            if len(w) != 2 or not (nav_dir / (w[1] + '.txt')).is_file():
                probs.append(f'line {n}: include {w[1:]} does not resolve under tools\\input_scripts\\nav')
    return probs, marks


def pwsh(*args):
    r = subprocess.run(['pwsh', '-NoProfile', '-File', *map(str, args)], capture_output=True, text=True, cwd=ROOT)
    return r.returncode, r.stdout + r.stderr


def parse_ps1(p):
    cmd = "$e=$null;[void][System.Management.Automation.Language.Parser]::ParseFile('%s',[ref]$null,[ref]$e);if($e){$e|%%{$_.Message};exit 1}" % str(p).replace("'", "''")
    r = subprocess.run(['pwsh', '-NoProfile', '-Command', cmd], capture_output=True, text=True)
    return r.returncode == 0, r.stdout.strip()[:200]


def marks_of(log):
    d = {}
    for l in open(log, encoding='utf-8', errors='replace'):
        m = re.search(r'scene=([A-Za-z0-9_]+)\b.*?\(frame (\d+)\)\s*$', l)
        if m and 'input' in l:
            d.setdefault(m.group(1), int(m.group(2)))
    return d


def p99(v):
    v = sorted(v)
    return v[min(len(v) - 1, int(0.99 * len(v)))]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=str(ROOT))
    a = ap.parse_args()
    root = Path(a.root).resolve()
    nav = root / 'tools' / 'input_scripts' / 'nav'
    route_p = root / 'tools' / 'input_scripts' / 'scenes' / 'bryan_heavy.txt'
    check(route_p.is_file(), 'route file exists: %s' % route_p.name)
    route = route_p.read_text(encoding='utf-8')
    known = known_commands(root)
    check(len(known) > 30 and {'wait_frames', 'mark', 'quit', 'include', 'wait_file', 'log', 'on_fail'} <= known, 'read %d commands from src\\input_script.h' % len(known))

    # 1. route
    probs, marks = lint_route(route, known, nav)
    check(not probs, 'route lints clean (%s)' % '; '.join(probs))
    md = dict(marks)
    check(md.get('tron_324') == 0 and md.get('heavy') == HEAVY0 and md.get('end') == HEAVY0 + HEAVYN and list(md) == ['tron_324', 'heavy', 'end'],
          'marks tron_324 +0, heavy +%d, end +%d in that order (got %s)' % (HEAVY0, HEAVY0 + HEAVYN, marks))
    check(re.search(r'^log scene=heavy', route, re.M) and re.search(r'^log scene=end', route, re.M) and re.search(r'^quit\s*$', route, re.M), 'route logs scene=heavy, scene=end and quits')
    hdr = re.search(r'^# harness: flags (.*)$', route, re.M)
    base = (root / 'tools' / 'input_scripts' / 'scenes' / 'bryan_entrance.txt').read_text(encoding='utf-8')
    bh = re.search(r'^# harness: flags (.*)$', base, re.M)
    check(hdr and bh and hdr.group(1) == bh.group(1), 'launch flags header equals bryan_entrance.txt')
    # the whole launch (before the tron mark) is the same lines as bryan_entrance.txt
    pre = lambda t: [l.split('#')[0].strip() for l in t.split('mark 1 tron_324')[0].splitlines() if l.strip() and not l.lstrip().startswith('#')]
    check(pre(route) == [x.replace('bryan_entrance', 'bryan_heavy') for x in pre(base)] or
          [x for x in pre(route) if 'bench: start' not in x] == [x for x in pre(base) if 'bench: start' not in x], 'launch part of the route equals bryan_entrance.txt (log text aside)')
    for name, bad, why in (('malformed number', route.replace('wait_frames 570', 'wait_frames 57O'), 'wait_frames wants one positive integer'),
                           ('unknown command', route.replace('wait_frames 570', 'wait_frame 570'), 'unknown command'),
                           ('bad include', route.replace('include boot_to_menu', 'include boot_to_menux'), 'does not resolve')):
        p, _ = lint_route(bad, known, nav)
        check(any(why in x for x in p), 'planted twin "%s" FAILs the lint with its reason' % name)
    p, m2 = lint_route(route.replace('mark 3 end\n', ''), known, nav)
    check([x for x, _ in m2] == ['tron_324', 'heavy'], 'planted twin "no end mark": the mark list loses end (bench_line refuses it in part 4)')

    # 2. wiring
    bp = (root / 'tools' / 'surface' / 'bench.ps1').read_text(encoding='utf-8')
    m = re.search(r"'frames_bryan_heavy' = @\('(\S+)', @\(([^)]*)\)\)", bp)
    mb = re.search(r"'frames_bryan' = @\('(\S+)', @\(([^)]*)\)\)", bp)
    check(m and m.group(1) == 'bryan_heavy.txt' and mb and m.group(2) == mb.group(2), "bench.ps1 $sceneSteps: frames_bryan_heavy -> bryan_heavy.txt with frames_bryan's flags")
    check("'bryan_heavy.txt'" in (root / 'tools' / 'surface' / 'push_kit.ps1').read_text(encoding='utf-8'), 'push_kit.ps1 ships bryan_heavy.txt in Routes')
    for f in ('bench.ps1', 'push_kit.ps1', 's0_5_jobs.ps1'):
        ok, why = parse_ps1(root / 'tools' / 'surface' / f)
        check(ok, '%s parses (%s)' % (f, why))
    tmp = Path(tempfile.mkdtemp(prefix='m6_'))
    rc, out = pwsh(root / 'tools/surface/s0_5_jobs.ps1', '-Out', tmp / 'o', '-CheckOnly', '-Jobs', 'h|Only=ab;Base=none;AbScene=frames_bryan_heavy;AbRounds=2;AbWarmup=true;ArmFlags=A:a=1|b:b=2')
    check(rc == 0 and 'job lines ok' in out, 's0_5_jobs.ps1 -CheckOnly accepts an AbScene=frames_bryan_heavy job line')
    check("frames_bryan_heavy" in (root / 'tools/surface/ab_line.py').read_text(encoding='utf-8'), 'ab_line.py --scene lists frames_bryan_heavy')

    # 3. the window from the real runs
    runs = sorted(AB8.glob('*/x/ab_*/frames_*.csv')) if AB8.is_dir() else []
    if not runs:
        print('SKIP  %s has no runs: parts 3-5 not run' % AB8)
        return 3 if not FAILS else 1
    data = []
    for c in runs:
        lg = c.with_name(c.name.replace('frames_', 'wwe2k14_').replace('.csv', '.log'))
        mk = marks_of(lg)
        if not all(k in mk for k in ('tron_324', 'step15', 'end')):
            continue
        rows = [r for r in csv.DictReader(open(c, newline='')) if r.get('frame') and r.get('presented') not in ('', '0', None) and r.get('present_gap_ms')]
        data.append((c, lg, mk, [(int(r['frame']), float(r['present_gap_ms'])) for r in rows]))
    check(len(data) >= 20, 'read %d real Surface runs with tron / step15 / end marks' % len(data))

    def pooled(sel):
        g = []
        for c, lg, mk, rows in data:
            f0, f1 = sel(mk)
            g += [x for f, x in rows if f0 <= f <= f1]
        return g
    full = pooled(lambda mk: (mk['tron_324'], mk['end']))
    old = pooled(lambda mk: (mk['step15'], mk['end']))
    heavy = pooled(lambda mk: (mk['tron_324'] + HEAVY0, mk['tron_324'] + HEAVY0 + HEAVYN))
    early = pooled(lambda mk: (mk['tron_324'] + 20, mk['tron_324'] + 620))
    slow = lambda g: sum(x >= SLOW_MS for x in g)
    tot = slow(full)
    mean = lambda g: sum(g) / len(g)
    for nm, g in (('whole entrance (tron..end)', full), ('old step15 window', old), ('heavy window', heavy), ('planted early window tron+20..+620', early)):
        print('      %-34s n=%5d mean %5.2f ms  p99 %5.1f ms  slow frames %4d = %4.1f %% of the entrance\'s %d (%.1f %% of its own gaps)' % (nm, len(g), mean(g), p99(g), slow(g), slow(g) / tot * 100, tot, slow(g) / len(g) * 100))
    check(slow(heavy) / tot >= 0.70, 'heavy window holds >= 70 %% of the slow frames (%.0f %%)' % (slow(heavy) / tot * 100))
    check(slow(old) / tot < 0.35, 'the old step15 window holds < 35 %% of them (%.0f %%): why it is replaced' % (slow(old) / tot * 100))
    check(p99(heavy) >= p99(full) - 0.5, 'heavy window p99 %.1f ms >= the whole entrance\'s %.1f ms (it contains the frames that set the 1 %% low)' % (p99(heavy), p99(full)))
    check(mean(heavy) >= mean(full), 'heavy window mean %.2f ms >= the whole entrance\'s %.2f ms (it is the heavy stretch)' % (mean(heavy), mean(full)))
    check(not (slow(early) / tot >= 0.70), 'planted twin: the early window tron+20..+620 holds only %.0f %% of the slow frames and FAILs the >= 70 %% assertion' % (slow(early) / tot * 100))
    check(len(heavy) / len(data) > len(old) / len(data), 'heavy window has more presents a run (%d) than the old (%d): a steadier 1 %% low' % (len(heavy) / len(data), len(old) / len(data)))

    # 4. bench_line / ab_line on a copy of a real run with the heavy route's marks
    c, lg, mk, rows = next(d for d in data if d[0].parent.name == 'ab_1_A' and d[0].parent.parent.parent.name == 'g1t11')
    T = mk['tron_324']
    h0, h1 = T + HEAVY0, T + HEAVY0 + HEAVYN
    src = c.parent
    lines = open(lg, encoding='utf-8', errors='replace').read().splitlines()
    stamp = lambda s: re.match(r'^(\[[^\]]+\])', s).group(1)
    s15 = next(l for l in lines if 'scene=step15' in l and 'input' in l)
    end = next(l for l in lines if 'scene=end' in l and 'input' in l)

    def make(root_t, name, drop_end=False, drop_heavy=False):
        d = root_t / name / 'frames_bryan_heavy'
        shutil.copytree(src, d)
        out = []
        for l in lines:
            if l is s15:
                if not drop_heavy:
                    out.append(re.sub(r'scene=step15 \(Bryan.s entrance step 15\) \(frame \d+\)', 'scene=heavy (tron + 760 frames) (frame %d)' % h0, l))
            elif l is end:
                if not drop_end:
                    out.append(re.sub(r'scene=end \(step 15 \+ 300 frames\) \(frame \d+\)', 'scene=end (heavy + 570 frames) (frame %d)' % h1, l))
            else:
                out.append(l)
        (d / lg.name).write_text('\n'.join(out) + '\n', encoding='utf-8')
        shutil.copystat(lg, d / lg.name)   # the rewritten log keeps the real log's mtime (heat_rules.check_fresh compares it with power.csv's)
        return d
    t2 = tmp / 'bl'
    d = make(t2, 'ok')
    r = subprocess.run([sys.executable, '-I', str(root / 'tools/surface/bench_line.py'), str(d), '--allow-no-heat'], capture_output=True, text=True)
    mm = re.search(r'bryan_heavy \[frames (\d+)\.\.(\d+)\]: shown ([\d.]+) fps, 1% low ([\d.]+)', r.stdout)
    check(r.returncode == 0 and mm and (int(mm.group(1)), int(mm.group(2))) == (h0, h1), 'bench_line.py reads the heavy route: window frames %d..%d (%s)' % (h0, h1, (mm.group(0) if mm else (r.stdout + r.stderr)[:200])))
    if mm:
        g = [x for f, x in rows if h0 <= f <= h1]
        low = 1000.0 / p99(g)
        check(abs(float(mm.group(4)) - low) < 0.06, 'its 1%% low %s equals the independent re-derivation from the csv (%.2f fps)' % (mm.group(4), low))
        check('bryan_heavy/' not in r.stdout, 'no second (tron) window is printed for the heavy route')
    for nm, kw, why in (('no scene=end', {'drop_end': True}, 'no scene=end'), ('no scene=heavy', {'drop_heavy': True}, 'no scene=heavy')):
        dd = make(t2, nm.replace(' ', '_'), **kw)
        r = subprocess.run([sys.executable, '-I', str(root / 'tools/surface/bench_line.py'), str(dd), '--allow-no-heat'], capture_output=True, text=True)
        check(r.returncode == 2 and why in r.stderr, 'planted twin "%s": bench_line.py REFUSES (rc %d)' % (nm, r.returncode))
    # ab_line: two arms (the same real run twice) under ab_1_A / ab_1_B
    abd = t2 / 'ab' / 'x'
    for arm in ('ab_1_A', 'ab_1_B'):
        shutil.copytree(make(t2, 'tmp_' + arm), abd / arm)   # make() returns the folder holding the run's files
    r = subprocess.run([sys.executable, '-I', str(root / 'tools/surface/ab_line.py'), str(abd), '--scene', 'frames_bryan_heavy'], capture_output=True, text=True)
    check(r.returncode == 0 and re.search(r'ab_1_A\s+shown', r.stdout) and 'no line' not in r.stdout, 'ab_line.py --scene frames_bryan_heavy prints the arms (%s)' % r.stdout.strip().splitlines()[0][:100])

    # 5. the estimate
    slot = []
    order = sorted(AB8.glob('*/x/ab_order.txt'))
    for o in order:
        ts = [l.split('\t')[2] for l in o.read_text().splitlines() if l.split('\t')[0] != 'abwarm']
        sec = [int(t[:2]) * 3600 + int(t[3:5]) * 60 + int(t[6:]) for t in ts]
        slot += [b - a for a, b in zip(sec, sec[1:]) if 0 < b - a < 200]
    slot = sorted(slot)[len(slot) // 2]
    swap = []
    for c, lg, mk, rows in data:
        swap += [x for f, x in rows if mk['tron_324'] + HEAVY0 + HEAVYN <= f <= mk['end']]
    saved_frames = data[0][2]['end'] - data[0][2]['tron_324'] - (HEAVY0 + HEAVYN)
    saved_s = saved_frames * mean(swap) / 1000.0
    new_slot = slot - saved_s
    print('      run slot today (median start-to-start in ab8, run + 20 s gap): %d s; window ends %d guest frames sooner = %.1f s (%.1f ms a frame there) -> %.0f s' % (slot, saved_frames, saved_s, mean(swap), new_slot))
    rc, out = pwsh(root / 'tools/surface/bench.ps1', '-AbPlan', '-AbRounds', '2', '-AbWarmup', '-ArmFlags', 'A:x=1|b:x=2|c:x=3|d:x=4|e:x=5|f:x=6|g:x=7')
    n = re.search(r'(\d+) runs', out)
    check(rc == 0 and n, 'bench.ps1 -AbPlan: 7 arms x 2 rounds = %s runs' % (n.group(1) if n else '?'))
    runs_n = int(n.group(1)) if n else 19
    print('      job of %d runs: %.1f min today, %.1f min with frames_bryan_heavy (%.0f %% shorter)' % (runs_n, runs_n * slot / 60, runs_n * new_slot / 60, saved_s / slot * 100))
    shutil.rmtree(tmp, ignore_errors=True)
    print('RESULT: %s' % ('FAIL (%d)' % len(FAILS) if FAILS else 'all checks passed'))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
