"""Session 101: the ownership picture. For a leftovers census output (tools\\census\\leftovers.py --grid WxH), draw the frame beside the same
frame with every grid cell the game still owns tinted pink (owner = the newest content draw behind the pixel, leftovers.py's own rule;
ours = any promoted fate: crowd_ours / crowd_injected / arena_ours / wrestlers_ours / ring_ours / flash_ours / props_ours / ui_ours /
particle_run(_head)). Game-derived output, local only (out\\).

  py -I tools\\census\\leftovers.py ZIP --frames 200 --promote auto --grid 320x180 --name NAME
  py -I tools\\census\\owner_overlay.py out\\s101\\leftovers\\NAME --frame 200 [--title TEXT]    -> NAME\\owner_sheet_f200.jpg
"""
import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import leftovers as L  # noqa: E402
from PIL import Image, ImageDraw  # noqa: E402

PINK = (255, 0, 160, 150)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('census_dir')
    ap.add_argument('--frame', type=int, required=True)
    ap.add_argument('--title', default='')
    a = ap.parse_args()
    d = Path(a.census_dir)
    meta = json.loads((d / 'leftovers.json').read_text(encoding='utf-8'))
    gx, gy = (int(v) for v in str(meta['grid']).lower().replace(' ', '').split('x')) if isinstance(meta['grid'], str) else meta['grid']
    pix = json.loads((d / f'judge_f{a.frame}' / 'pixels.json').read_text(encoding='utf-8'))
    draws = L.load_draws(d / f'ops_f{a.frame}.json')[0]
    draws = L.mark_particle_heads(draws)  # S101 lane 125: the return value was dropped (heads of merged particle runs read as stream: frame 260 98.5% instead of 46.1%)
    fate = {x['op']: x.get('fate', 'stream') for x in draws}
    owners = L.owners(pix['pixels'])
    if len(owners) != gx * gy:
        sys.exit(f'grid {gx}x{gy} does not match {len(owners)} samples')
    pic = Image.open(d / f'judge_f{a.frame}' / 'picture.png').convert('RGB')
    over = pic.copy()
    dr = ImageDraw.Draw(over, 'RGBA')
    cw, ch = pic.size[0] / gx, pic.size[1] / gy
    ours = stream = 0
    for i, (o, _) in enumerate(owners):
        if o is None:
            continue
        if fate.get(o, 'stream') in ('stream', None):
            stream += 1
            x, y = i % gx, i // gx
            dr.rectangle([x * cw, y * ch, (x + 1) * cw, (y + 1) * ch], fill=PINK)
        else:
            ours += 1
    share = 100 * stream / max(1, ours + stream)
    s = Image.new('RGB', (1920, 580), 'black')
    s.paste(pic.resize((960, 540)), (0, 20))
    s.paste(over.resize((960, 540)), (960, 20))
    t = ImageDraw.Draw(s)
    # lane 143: a census that is not proven (incomplete frame, or the last-frame proof bar failed) says so ON the sheet, in red
    fr = next((x for x in meta['frames'] if x['frame'] == a.frame), {})
    warn = []
    if fr.get('complete') is False:
        warn.append('INCOMPLETE FRAME: ' + fr.get('incomplete_why', ''))
    pb = meta.get('proof_bar')
    if pb is None:
        warn.append('no proof bar in this census (made before lane 143)')
    elif pb.get('ok') is not True:
        warn.append('PROOF BAR ' + ('FAILED: the redraw of the last frame does not match the live picture' if pb.get('ok') is False else 'NOT RUN'))
    if warn:
        t.rectangle([0, 560, 1920, 580], fill=(150, 0, 0))
        t.text((6, 564), 'NOT A MEASUREMENT (unproven): ' + ' | '.join(warn), fill='white')
    t.text((6, 4), a.title or f'{d.name}, frame {a.frame}: drawn with the promotions on', fill='yellow')
    t.text((966, 4), f'pink = still drawn by the game ({share:.1f}%); everything else ours', fill='yellow')
    out = d / f'owner_sheet_f{a.frame}.jpg'
    s.save(out, quality=92)
    print(f'ours {ours}, stream {stream}, game share {share:.1f}% -> {out}')
    if warn:
        print('FAIL UNPROVEN: ' + ' | '.join(warn))
        sys.exit(3)


if __name__ == '__main__':
    main()
