"""S101 lane 4: distinct effect names in atlas.json (loaded meshes, skipped meshes, and the 0xAC-variant models' names) vs src/renderer/shaders/fx/<name>.fx.json."""
import glob, json, os, re, sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
atlas = sys.argv[1] if len(sys.argv) > 1 else str(ROOT.parents[2] / 'out' / 'atlas' / 'atlas.json')
loaded, skipped, variant = set(), set(), set()
for l in open(atlas, encoding='utf-8'):
    l = l.strip().rstrip(',')
    if not l.startswith('{'):
        continue
    r = json.loads(l)
    if r['type'] == 'mesh':
        (loaded if r['loaded'] else skipped).add(r['effect'])
    elif r['type'] == 'unparsed' and r['what'] == 'model':
        m = re.search(r'effects at \+0x78: ([^)]*)\)', r['reason'])
        if m:
            variant |= set(m.group(1).split(','))
have = {os.path.basename(p)[:-8] for p in glob.glob(str(ROOT / 'src/renderer/shaders/fx/*.fx.json'))}
print('loaded', sorted(loaded))
print('skipped-only', sorted(skipped - loaded))
print('0xAC-variant models', sorted(variant))
print('no fx.json:', [n for n in sorted(loaded | skipped | variant) if n not in have])
