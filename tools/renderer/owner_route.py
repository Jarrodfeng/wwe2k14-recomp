"""Owner-mode twin of an input-script route, for the new renderer's like-for-like checks (Session 100).

Every `screenshot NAME` line (and `screenshot_every N NAME`) gets a `cvar wwe2k14_rdr_live_picture NAME` line before it, so the new renderer
writes out\\captures\\live\\live_NAME.png of the frame the reference build screenshots. `include` lines are kept (the harness resolves them).
The twin goes next to the route as s100_owner_<name>.txt unless --out says otherwise. Run the twin with --wwe2k14_rdr_owner=true and the
original on the frozen reference build (out\\release\\stage\\wwe2k14-recomp-v0.3.1\\wwe2k14.exe) with the same flags, then compare:
    py tools\\renderer\\owner_route.py tools\\input_scripts\\scenes\\bryan_entrance.txt
    py tools\\renderer\\owner_route.py --compare REF_CAPTURES_DIR RUN_NUMBER [--live out\\captures\\live]
"""
import argparse
import pathlib
import re
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont


def twin(route: pathlib.Path, out: pathlib.Path | None) -> pathlib.Path:
    lines = route.read_text(encoding='utf-8').splitlines()
    result = []
    names = []
    for line in lines:
        m = re.match(r'\s*screenshot\s+(\S+)\s*$', line)
        if m:
            names.append(m.group(1))
            result.append(f'cvar wwe2k14_rdr_live_picture {m.group(1)}')
            result.append(line)
            continue
        if re.match(r'\s*quit\s*$', line):
            # The live picture is written a frame or more after its cvar; a route that quits right after its last screenshot would end
            # first. Before the quit, so no shot moves against the reference.
            result.append('# (owner twin) time for the last live picture')
            result.append('wait_frames 10')
        w = re.match(r'(\s*wait_file\s+\S+\s+)(\d+)(.*)$', line)
        if w:
            # Owner mode fast-forwards several times faster (the old path draws nothing): a load measured in frames needs more of them.
            result.append('# (owner twin) wait x4: ' + line.strip())
            result.append(f'{w.group(1)}{int(w.group(2)) * 4}{w.group(3)}')
            continue
        elif re.match(r'\s*screenshot_every\s', line):
            print(f'note: {line.strip()} kept as is (the live picture is one frame per cvar line)', file=sys.stderr)
        elif re.match(r'\s*cvar\s+gpu_native_owned_frame(?:\s|$)', line):
            # The old renderer's owned-frame switch: meaningless with the old path drawing nothing, and flipping it while the reader runs
            # crashed WM30 twice (S100 runs 6861 / 6864, render_cmd_replay_lists): left out of twins.
            result.append('# (owner twin) left out: ' + line.strip())
            continue
        result.append(line)
    head = 1 if lines and lines[0].startswith('# harness:') else 0
    note = [f'# Session 100: owner-mode twin of {route.name} (tools\\renderer\\owner_route.py): the new renderer writes live_<name>.png of',
            '# every screenshot the reference build takes. Run with --wwe2k14_rdr_owner=true.']
    result = result[:head] + note + result[head:]
    out = out or route.with_name('s100_owner_' + route.name)
    out.write_text('\n'.join(result) + '\n', encoding='utf-8')
    print(f'{out}: {len(names)} pictures: {", ".join(names)}')
    return out


def compare(ref_dir: pathlib.Path, run: str, live: pathlib.Path, sheet: pathlib.Path | None) -> None:
    pairs = []
    for ref in sorted(ref_dir.glob(f'{run}_*.png')):
        name = ref.stem[len(run) + 1:]
        ours = live / f'live_{name}.png'
        if not ours.exists():
            print(f'{name}: no live picture ({ours})')
            continue
        a = np.asarray(Image.open(ref).convert('RGB'), dtype=np.float32)
        b = np.asarray(Image.open(ours).convert('RGB').resize((a.shape[1], a.shape[0])), dtype=np.float32)
        qs = [5, 20, 50, 80, 95]
        worst = max(abs(np.percentile(a[..., c], q) - np.percentile(b[..., c], q)) for c in range(3) for q in qs)
        means = ' '.join(f'{a[..., c].mean():.0f}/{b[..., c].mean():.0f}' for c in range(3))
        print(f'{name}: mean R/G/B game/ours {means}; worst quantile gap {worst:.0f}')
        pairs.append((name, ref, ours))
    if sheet and pairs:
        w, h = 640, 360
        img = Image.new('RGB', (2 * w + 10, len(pairs) * (h + 26)), (20, 20, 20))
        d = ImageDraw.Draw(img)
        try:
            font = ImageFont.truetype('arial.ttf', 18)
        except OSError:
            font = None
        for i, (name, ref, ours) in enumerate(pairs):
            y = i * (h + 26)
            for c, (p, label) in enumerate([(ref, 'GAME (reference)'), (ours, 'NEW RENDERER')]):
                img.paste(Image.open(p).convert('RGB').resize((w, h), Image.LANCZOS), (c * (w + 10), y + 26))
                d.text((c * (w + 10) + 6, y + 3), f'{label}  {name}', fill=(255, 255, 255), font=font)
        img.save(sheet)
        print(f'sheet: {sheet}')


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument('route', nargs='?')
    ap.add_argument('--out')
    ap.add_argument('--compare', nargs=2, metavar=('REF_CAPTURES_DIR', 'RUN'))
    ap.add_argument('--live', default='out/captures/live')
    ap.add_argument('--sheet')
    args = ap.parse_args()
    if args.compare:
        compare(pathlib.Path(args.compare[0]), args.compare[1], pathlib.Path(args.live), pathlib.Path(args.sheet) if args.sheet else None)
    elif args.route:
        twin(pathlib.Path(args.route), pathlib.Path(args.out) if args.out else None)
    else:
        ap.error('a route, or --compare')


if __name__ == '__main__':
    main()
