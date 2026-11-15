r"""Session 102 lane B1 offline test (0 game runs, nothing on screen): the burst-aware thermal verdict (tools\surface\burst_rules.py, R28).
  py -I tools\tests\burst_rules_test.py [--root DIR]
Parts:
  A  made-up power.csv files read through read_power + classify: a burst tail (the credit ends inside the run), a sustained run, a mixed one (the burst ended
     3 s before the window; a warm chip above the band), a cool one (a menu after idle), a file with no cpu_perf column;
  B  planted twins: ONE burst sample inside a sustained window must flip SUSTAINED to BURST; the same sample 3 s BEFORE the window must give MIXED, 7 s before
     (outside the 5 s lookback) must stay SUSTAINED; a window sample at 15.6 W alone (perf 110) is a burst sample, at 15.4 W is not;
  C  the normalised fps (fps x (110 / perf)^0.5) on R28's own pair: bar5 Bryan 53.1 fps at 131.5 % -> 48.5, bar6 47.2 at 101.9 % -> 49.0;
  D  the real folders (out\s102\ab27\bar5, bar6) when they are on this machine: bar5 Bryan must read BURST, bar6 Bryan SUSTAINED (skipped, said so, when absent).
Exit 0 only when every check passes."""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
if '--root' in sys.argv:
    ROOT = Path(sys.argv[sys.argv.index('--root') + 1]).resolve()
sys.path.insert(0, str(ROOT / 'tools' / 'surface'))
import burst_rules as br  # noqa: E402

OUT = ROOT / 'out' / 'b1'
OUT.mkdir(parents=True, exist_ok=True)
fails = []


def check(cond, msg):
    print(('ok   ' if cond else 'FAIL ') + msg)
    if not cond:
        fails.append(msg)


def make(name, profile, n=60, t0=10 * 3600, with_perf=True):
    """profile(i) -> (perf %, package W) for second i. Writes a power.csv shaped like the sampler's (milliwatts)."""
    p = OUT / name
    with open(p, 'w', newline='') as f:
        f.write('time,t_s,' + ('cpu_perf,' if with_perf else '') + 'rapl_pkg\n')
        for i in range(n):
            t = t0 + i
            perf, w = profile(i)
            f.write(f'{t // 3600:02d}:{t % 3600 // 60:02d}:{t % 60:02d}.100,{i + 1.0},' + (f'{perf:.2f},' if with_perf else '') + f'{w * 1000:.2f}\n')
    return p


def verdict(p, a, b, t0=10 * 3600):
    return br.classify(br.read_power(p), t0 + a, t0 + b)


T0 = 10 * 3600
# A: shapes
tail = make('burst_tail.csv', lambda i: (145.0, 22.0) if i < 38 else (108.0, 13.8))             # burst until 38 s, the window 30..45 straddles its end
v = verdict(tail, 30, 45)
check(v['state'] == 'BURST' and v['n_burst'] == 8 and v.get('lead') == 8, f'A burst tail: BURST, 8 burst seconds, a tail of 8 (got {v["state"]}, {v["n_burst"]}, lead {v.get("lead")})')
check('burst tail' in v['why'], 'A burst tail: the reason names it a tail')
v = verdict(tail, 5, 25)
check(v['state'] == 'BURST' and v['n_burst'] == 20, f'A inside the burst: BURST ({v["state"]})')
sus = make('sustained.csv', lambda i: (105.0 + (i % 3) * 3, 13.2 + (i % 4) * 0.3))
v = verdict(sus, 20, 40)
check(v['state'] == 'SUSTAINED', f'A sustained: SUSTAINED ({v["state"]}: {v["why"]})')
sag = make('sag.csv', lambda i: (102.0, 12.6))     # the 40-60 min sag of R28 (13.0 W / 102 % reads sustained, 12.6 W is its low end)
check(verdict(sag, 20, 40)['state'] == 'SUSTAINED', 'A sagged plateau 12.6 W / 102 %: SUSTAINED')
mix1 = make('mixed_after.csv', lambda i: (146.0, 22.0) if i < 17 else (108.0, 13.8))             # burst ended 3 s before the window (starts at 20)
v = verdict(mix1, 20, 40)
check(v['state'] == 'MIXED' and v['n_burst'] == 0 and v['n_burst_pre'] >= 1, f'A mixed (burst ended just before the window): MIXED ({v["state"]})')
warm = make('warm.csv', lambda i: (118.0, 14.4))        # no burst sample, mean above the band (perf 118 > 115)
check(verdict(warm, 20, 40)['state'] == 'MIXED', 'A warm chip above the band (118 %, 14.4 W): MIXED')
cool = make('cool_idle.csv', lambda i: (55.0, 3.1))     # a menu after idle: far under the plateau
check(verdict(cool, 20, 40)['state'] == 'COOL', 'A cool chip after idle (55 %, 3.1 W): COOL')
noperf = make('noperf.csv', lambda i: (0, 13.5), with_perf=False)
check(verdict(noperf, 20, 40)['state'] == 'NONE', 'A no cpu_perf column: NONE (never SUSTAINED by default)')
check(verdict(sus, 100, 120)['state'] == 'NONE', 'A window outside the file: NONE')

# B: planted twins
twin_in = make('twin_inside.csv', lambda i: (146.0, 21.0) if i == 30 else (105.0 + (i % 3) * 3, 13.2 + (i % 4) * 0.3))
check(verdict(twin_in, 20, 40)['state'] == 'BURST', 'B twin: ONE burst sample inside the window flips SUSTAINED to BURST')
check(verdict(sus, 20, 40)['state'] == 'SUSTAINED', 'B twin base (same file without the sample): SUSTAINED')
twin_pre3 = make('twin_pre3.csv', lambda i: (146.0, 21.0) if i == 17 else (105.0 + (i % 3) * 3, 13.2 + (i % 4) * 0.3))
check(verdict(twin_pre3, 20, 40)['state'] == 'MIXED', 'B twin: the burst sample 3 s before the window: MIXED')
twin_pre7 = make('twin_pre7.csv', lambda i: (146.0, 21.0) if i == 13 else (105.0 + (i % 3) * 3, 13.2 + (i % 4) * 0.3))
check(verdict(twin_pre7, 20, 40)['state'] == 'SUSTAINED', 'B twin: the burst sample 7 s before the window (outside the 5 s lookback): SUSTAINED')
hi_w = make('twin_w156.csv', lambda i: (110.0, 15.6) if i == 30 else (110.0, 13.8))
check(verdict(hi_w, 20, 40)['state'] == 'BURST', 'B twin: one sample at 15.6 W (> 15.5) is a burst sample')
lo_w = make('twin_w154.csv', lambda i: (110.0, 15.4) if i == 30 else (110.0, 13.8))
check(verdict(lo_w, 20, 40)['state'] == 'SUSTAINED', 'B twin: one sample at 15.4 W (under 15.5, window mean in the band) is not')
hi_p = make('twin_p125.csv', lambda i: (125.0, 14.0) if i == 30 else (108.0, 13.8))
check(verdict(hi_p, 20, 40)['state'] == 'BURST', 'B twin: one sample at cpu_perf 125 % is a burst sample')

# C: normalisation on R28's pair
n5, n6 = br.normalised(53.1, 131.5), br.normalised(47.2, 101.9)
check(abs(n5 - 48.5) < 0.1 and abs(n6 - 49.0) < 0.1, f'C normalised 53.1 @131.5 -> {n5:.2f}, 47.2 @101.9 -> {n6:.2f} (R28: 48.5 and 49.0)')
check(br.normalised(50.0, None) is None and br.normalised(None, 110.0) is None and br.normalised(50.0, 0) is None, 'C no perf / no fps: None')
check(abs(br.normalised(50.0, 110.0) - 50.0) < 1e-9, 'C at 110 % the fps is unchanged')
check(br.text(v).startswith('thermal ') and '|' not in br.text(v) and '[' not in br.text(v), 'C the printed suffix has no | and no [ (ab_line.py reads the heat part by regex)')

# D: real folders
BL = ROOT / 'tools' / 'surface' / 'bench_line.py'
MAIN = Path(r'C:\Users\Jarrod\wwe2k14-recomp\out\s102\ab27')


def real(folder):
    r = subprocess.run([sys.executable, '-I', str(BL), str(MAIN / folder), '--allow-no-heat'], capture_output=True, text=True)
    out = r.stdout
    first = out.split(' | bryan/tron')[0] if ' | bryan/tron' in out else out   # the step15 window only
    import re
    m = re.search(r'thermal (\w+)', first)
    return m.group(1) if m else None


if (MAIN / 'bar5' / 'x' / 'frames_bryan').is_dir() and (MAIN / 'bar6' / 'x' / 'frames_bryan').is_dir():
    check(real(r'bar5\x\frames_bryan') == 'BURST', 'D real bar5 Bryan step15 window reads BURST')
    check(real(r'bar6\x\frames_bryan') == 'SUSTAINED', 'D real bar6 Bryan step15 window reads SUSTAINED')
else:
    print('skip D: out\\s102\\ab27\\bar5 / bar6 are not on this machine')

print('FAILED: ' + '; '.join(fails) if fails else 'ALL OK')
sys.exit(1 if fails else 0)
