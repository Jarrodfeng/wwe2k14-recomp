"""S101 lane 23: which blend states do the game's ySprite draws (the 2D layer) use? Reads capture frames (format: tools/renderer/cap_draws.py),
counts draws whose pixel shader is one of ySprite.fx.json's, grouped by (pixel-shader option, blend0). Counts only: nothing from the game is written.
usage: py -I tools/pass2d/sprite_blend_census.py CAPTURE_DIR [CAPTURE_DIR ...]"""
import collections
import json
import pathlib
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
fx = json.loads((ROOT / 'src/renderer/shaders/fx/ySprite.fx.json').read_text())
ps_name = {int(h, 16): v['name'] for v in fx['fragment'] for h in v['hashes']}
FACT = {0: 'Zero', 1: 'One', 2: 'SrcColor', 3: '1-SrcColor', 4: 'DstColor', 5: '1-DstColor', 6: 'SrcAlpha', 7: '1-SrcAlpha', 8: 'DstAlpha', 9: '1-DstAlpha'}
tot = collections.Counter()
for cap in map(pathlib.Path, sys.argv[1:]):
    for f in sorted(cap.joinpath('frames').glob('frame_*.bin')):
        b = f.read_bytes()
        _, _, cf, sc, gf, pi, fl = struct.unpack_from('<IIIIQII', b, 0)
        S = {}
        for s in range(sc):
            sid, es, cnt, so = struct.unpack_from('<IIQQ', b, 32 + 24 * s)
            S[sid] = (es, cnt, so)
        es, cnt, so = S[4]
        for i in range(cnt):
            vs, ps, vsp, fixed, dyn, tf, tc, vb, pb = struct.unpack_from('<QQQIIIIII', b, so + es * i)
            if ps not in ps_name:
                continue
            fes, _, fso = S[7]
            en, sr, ds, op, sa, da, oa, wm = struct.unpack_from('<8B', b, fso + fes * fixed + 24)
            desc = f'enable={en}'
            if en:
                desc += f' rgb {FACT.get(sr, sr)}/{FACT.get(ds, ds)} op{op} a {FACT.get(sa, sa)}/{FACT.get(da, da)} op{oa}'
            tot[(ps_name[ps], desc)] += 1
for (n, k), c in sorted(tot.items(), key=lambda x: -x[1]):
    print(f'{c:6d}  {n:22s} {k}')
print('total ySprite draws', sum(tot.values()))
