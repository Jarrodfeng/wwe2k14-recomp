# Session 95 (TILER): count poison-coloured pixels in a run's screenshots (owned_check -Poison).
#   py -I tools\native\magenta_scan.py [--dir EXEDIR] RUN [RUN...]
# gpu_tiler_poison clears the DONT_CARE loads of the owned passes to magenta (1,0,1,1) / depth 0.0. A pass that relied on kept contents shows magenta
# (or a missing depth test) in the picture. Per shot: pixels with R,B >= 235 and G <= 25 and the largest 32x32 block's count; any such pixel is listed
# as a FINDING (the game's own pink UI is rare; look at the shot). Exit 1 when a shot has any poison pixel, or when no shots are found.
import sys
from pathlib import Path
from PIL import Image
ROOT = Path(__file__).resolve().parents[2]
CAPTURES = ROOT / 'out' / 'build' / 'win-amd64-release' / 'captures'
args = sys.argv[1:]
if '--dir' in args:
    i = args.index('--dir')
    CAPTURES = Path(args[i + 1]) / 'captures'
    del args[i:i + 2]
bad = 0
missing = 0
for run in args:
    files = sorted(CAPTURES.glob(f'{run}_*.png')) or sorted(CAPTURES.glob(f'{int(run):03d}_*.png'))
    if not files:
        print(f'run {run}: no shots in {CAPTURES}')
        missing += 1
        continue
    for f in files:
        im = Image.open(f).convert('RGB')
        w, h = im.size
        px = im.load()
        n = 0
        blocks = {}
        for y in range(h):
            for x in range(w):
                r, g, b = px[x, y]
                if r >= 235 and b >= 235 and g <= 25:
                    n += 1
                    k = (x // 32, y // 32)
                    blocks[k] = blocks.get(k, 0) + 1
        top = max(blocks.items(), key=lambda kv: kv[1]) if blocks else None
        flag = 'FINDING' if n > 0 else 'clean'
        print(f'{f.name}: {n} poison pixels of {w * h} ({flag})' + (f', densest 32px block at x{top[0][0] * 32} y{top[0][1] * 32}: {top[1]}' if top else ''))
        bad += n > 0
sys.exit(1 if bad or missing else 0)
