# S102 M2: a stand-in for rplay.exe, used only by lab.py's fake host (the lab's own offline test). Writes a --timing csv in rplay's format from made-up numbers
# and ends with rplay's closing "[rplay] capture: N frames" line. Test hooks (flags): --fake-exit N exits N; --fake-sleep S sleeps S seconds first; --fake-nogpu writes
# gpu_ns = 0; --fake-nogpu-after R zeroes gpu_ns from repeat R on (repeat read from the _rK in the csv name); --fake-zero-some zeroes one body frame; --fake-short
# writes 3 rows fewer than it reports; --fake-nocsv writes nothing; --fake-hang sleeps 60 s; --fake-passes writes the per-pass file. A file <capture>/frames_limit.txt
# caps the frame count like a short capture does.
import re, sys, time
from pathlib import Path

args = sys.argv[1:]
def val(name, default=None):
    return args[args.index(name) + 1] if name in args and args.index(name) + 1 < len(args) else default

cap = val('--capture')
if not cap or not Path(cap).is_dir():
    print(f'[rplay] cannot open capture {cap}')
    sys.exit(3)
if '--fake-hang' in args:
    time.sleep(60)
if '--fake-sleep' in args:
    time.sleep(float(val('--fake-sleep')))
code = int(val('--fake-exit', 0))
if code:
    print('[rplay] fake failure')
    sys.exit(code)
out = val('--timing')
frames = int(val('--frames', 3))
limit = Path(cap) / 'frames_limit.txt'
if limit.is_file():
    frames = min(frames, int(limit.read_text().strip()))
if '--fake-nocsv' in args:
    print(f'[rplay] capture: {frames} frames, 100 draws')
    sys.exit(0)
rep = re.search(r'_r(\d+)\.csv$', out or '')
repeat = int(rep.group(1)) if rep else 1
gpu_scale = 0 if ('--fake-nogpu' in args or ('--fake-nogpu-after' in args and repeat >= int(val('--fake-nogpu-after')))) else 1
written = frames - 3 if '--fake-short' in args else frames
with open(out, 'w') as f:
    f.write('frame,draws,load_us,bind_us,render_us,gpu_us,gpu_ns\n')
    for i in range(written):
        first = 5.0 if i == 0 else 1.0
        scale = 0 if ('--fake-zero-some' in args and i == 2) else gpu_scale
        f.write(f'{i},100,{200 * first:.1f},{300 * first:.1f},{900 * first:.1f},{12000 + 10 * (i % 5):.1f},{(10_000_000 + 1000 * (i % 7)) * scale}\n')
if '--fake-passes' in args:
    with open(out + '.passes.csv', 'w') as f:
        f.write('frame,pass,gpu_ns\n')
        for i in range(frames):
            f.write(f'{i},shadow,2000000\n{i},main,8000000\n')
print(f'[rplay] capture: {frames} frames, 100 draws, 100 bound, 0 missing a shader')
