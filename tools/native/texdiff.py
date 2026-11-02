"""Per-texture comparator, the report half (Session 82, harness builder): reads texdiff_rdc.py's result.json and prints
every texture the scene pass samples whose contents differ between the owned-off and owned-on captures, plus a picture
sheet (off | on | difference) for the RGBA8-like ones.

    py -I tools\\native\\texdiff.py OUTDIR [--top N]
"""
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

RGBA = ('R8G8B8A8', 'B8G8R8A8', 'R8G8B8A8_UNORM', 'B8G8R8A8_UNORM')


def decode(fmt, w, h, data):
    """RGBA8-like texture bytes -> RGB uint8 array, or None."""
    f = fmt.upper().replace('_UNORM', '').replace('_SRGB', '')
    if f not in ('R8G8B8A8', 'B8G8R8A8') or len(data) < w * h * 4:
        return None
    a = np.frombuffer(data[:w * h * 4], np.uint8).reshape(h, w, 4)
    return a[:, :, [2, 1, 0]] if f == 'B8G8R8A8' else a[:, :, :3]


def main():
    args = sys.argv[1:]
    top = 25
    if '--top' in args:
        i = args.index('--top')
        top = int(args[i + 1])
        del args[i:i + 2]
    out = Path(args[0])
    if (out / 'FAILED').exists():
        print('texdiff: the RenderDoc analysis failed - see', out / 'analyze.log')
        return 1
    res = json.loads((out / 'result.json').read_text())
    print(f"scene draws: off {res['scene_draws_off']}, on {res['scene_draws_on']}; texture uses: off {res['uses_off']}, on "
          f"{res['uses_on']}; distinct textures: off {res['distinct_textures_off']}, on {res['distinct_textures_on']}")
    print(f"uses with the SAME key in both whose contents DIFFER: {res['mismatching_uses']} in {len(res['groups'])} distinct "
          f"textures; uses in one capture only: off {res['only_off_count']}, on {res['only_on_count']}")
    real = [g for g in res['groups'] if not g.get('reordered')]
    moved = [g for g in res['groups'] if g.get('reordered')]
    if moved:
        print(f"  {len(moved)} of those only moved: both of their hashes are sampled elsewhere in the other capture (a draw-order shift, "
              f"not a content difference); not listed")
    for key in ('only_off_content', 'only_on_content'):
        if res.get(key):
            print(f"  textures (by content) sampled in {key[5:-8]} only ({len(res[key])}): " + '; '.join(res[key][:12]))
    res['groups'] = real
    tiles = []
    for g in res['groups'][:top]:
        share = g.get('differing_byte_share')
        where = ''
        if share is not None:
            row = g['first_diff_byte'] // max(1, g['bytes'] // max(1, g['h'])) if g['h'] else 0
            where = f", {100 * share:.2f}% of bytes differ, rows ~{row}.."
            last = g['last_diff_byte'] // max(1, g['bytes'] // max(1, g['h'])) if g['h'] else 0
            where += str(last)
        size_note = '' if (g['on_w'], g['on_h'], g['on_fmt']) == (g['w'], g['h'], g['fmt']) else \
            f" (ON has {g['on_w']}x{g['on_h']} {g['on_fmt']})"
        print(f"  #{g['idx']}: {g['w']}x{g['h']} {g['fmt']} x{g['arr']} mips {g['mips']}: {g['uses']} use(s), first at "
              f"{g['stage']}s slot {g['slot']} of ps {g['ps']} (draw off {g['first_off_draw']} / on {g['first_on_draw']}); "
              f"hash off {g['off_md5'][:8]} on {g['on_md5'][:8]}{where}{size_note}")
        a, b = out / 'bins' / f"g{g['idx']}_off.bin", out / 'bins' / f"g{g['idx']}_on.bin"
        if a.exists() and b.exists():
            ia, ib = decode(g['fmt'], g['w'], g['h'], a.read_bytes()), decode(g['fmt'], g['w'], g['h'], b.read_bytes())
            if ia is not None and ib is not None:
                tiles.append((g, ia, ib))
    for key in ('only_off', 'only_on'):
        items = res[key]
        if items:
            print(f"  {key.replace('_', ' ')} ({res[key + '_count']} uses; first {min(len(items), 8)}):")
            for e in items[:8]:
                print(f"    {e['w']}x{e['h']} {e['fmt']} {e['stage']}s slot {e['slot']} ps {e['ps']} draw {e['draw']}")
    if tiles:
        tw = 320
        rows = []
        for g, ia, ib in tiles[:12]:
            th = max(1, round(g['h'] * tw / g['w']))
            d = np.abs(ia.astype(np.int16) - ib.astype(np.int16)).max(axis=2)
            heat = np.zeros(d.shape + (3,), np.uint8)
            heat[(d >= 1) & (d <= 4)] = (40, 70, 200)
            heat[(d > 4) & (d <= 8)] = (255, 170, 30)
            heat[d > 8] = (255, 30, 30)
            ims = [Image.fromarray(x).resize((tw, th), Image.NEAREST) for x in (ia, ib, heat)]
            rows.append((f"#{g['idx']} {g['w']}x{g['h']} {g['fmt']} {g['uses']} use(s)", ims, th))
        sheet = Image.new('RGB', (3 * tw + 8, sum(r[2] + 16 for r in rows) + 24), (40, 40, 40))
        dr = ImageDraw.Draw(sheet)
        dr.text((4, 4), 'owned OFF | owned ON | difference (blue 1-4, orange 5-8, red > 8)', fill=(255, 255, 255))
        y = 24
        for label, ims, th in rows:
            dr.text((4, y), label, fill=(255, 255, 0))
            for c, im in enumerate(ims):
                sheet.paste(im, (c * (tw + 4), y + 14))
            y += th + 16
        p = out / 'sheet.jpg'
        sheet.save(p, 'JPEG', quality=88)
        print('sheet:', p)
    print('TEXDIFF: %d distinct texture(s) differ in content (by hash; %d only moved)' % (len(res['groups']), len(moved)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
