"""Review pack for a look-changing renderer item: labelled before / after pictures and a short clip for the owner.

Runs the same replay route twice (s21_lb replay, LOW/NATIVE preset): BASE, and BASE + the item's flag, through
tools\\bench\\run_list.ps1 (run lock, frozen user data, two lanes in parallel). The route is tools\\s66\\review_shots.txt:
the six s45_parity_shots same-frame pictures (Brock +200, Bryan, match +600 / 1500 / 2400 / 3300) plus a frame-locked
clip (300 guest frames = 5 s at 60 fps, 720p) of Brock's entrance crowd. Then, under out\\s66\\review\\<ITEM>\\ only:
  <shot>_pair.png   BEFORE (base) | AFTER: <flag>, full size, labelled (side_by_side.py's label style)
  <shot>_crop.png   2x magnified crop of the most-changed region of that shot, both arms, labelled
  clip.mp4          the two clips side by side (ffmpeg hstack + drawtext labels, 60 fps, h264), with --no-clip skipped
  index.html        flag, base flags, run numbers, every picture, the clip, a table for the numbers (placeholder)
  summary.txt       run numbers, changed-pixel shares per shot, timings
Replay frames are near- not byte-identical (two-state frames): the 'over 8 levels' share is only meaningful next to an
A/A pair. Nothing is copied into the repository; the game's pictures stay under out\\.

    py tools\\s66\\review_pack.py ITEM --flag "--x=1 --y=2" [--base "--z=3"] [--no-clip] [--runs BASE_RUN,FLAG_RUN]

--runs reuses two finished runs of the route (skips the game launches). Every run passes
--wwe2k14_settings_file=out\\s66\\settings_test.json. Exit 0 = pack written.
"""
import argparse
import glob
import html
import os
import re
import shutil
import subprocess
import sys
import time

import numpy as np
from PIL import Image, ImageDraw, ImageFont

R = r'C:\Users\Jarrod\wwe2k14-recomp'
CAPS = R + r'\out\build\win-amd64-release\captures'
ROUTE = R + r'\tools\s66\review_shots.txt'
SETTINGS = R + r'\out\s66\settings_test.json'


def label(img, text, size=None):
    d = ImageDraw.Draw(img)
    try:
        font = ImageFont.truetype('arialbd.ttf', size or max(18, img.height // 30))
    except OSError:
        font = ImageFont.load_default()
    box = d.textbbox((0, 0), text, font=font)
    pad = 8
    d.rectangle((0, 0, box[2] + 2 * pad, box[3] + 2 * pad), fill=(0, 0, 0))
    d.text((pad, pad), text, fill=(255, 255, 255), font=font)
    return img


def run_arms(common, base, flag, outdir):
    """Both arms in one run_list call, two lanes. Returns (base_run, flag_run)."""
    def q(s):
        return "'" + s.replace("'", "''") + "'"
    ps = os.path.join(outdir, '_run.ps1')
    clipdir = os.path.join(outdir, 'clips_raw')
    os.makedirs(clipdir, exist_ok=True)
    c = f'--wwe2k14_replay_play=s21_lb --wwe2k14_preset_low=true --wwe2k14_settings_file={SETTINGS} ' \
        f'--wwe2k14_clip_dir={clipdir} --wwe2k14_clip_frame_locked=true {base}'.strip()
    with open(ps, 'w') as f:
        f.write(f"& '{R}\\tools\\bench\\run_list.ps1' -Scripts @({q(ROUTE)}, {q(ROUTE)}) "
                f"-ScriptExtras @({q(c)}, {q(c + ' ' + flag)}) -Parallel 2 -WaitSeconds 1500\n")
    out = subprocess.run(['pwsh', '-NoProfile', '-File', ps], capture_output=True, text=True, encoding='utf-8',
                         errors='replace').stdout
    runs = re.findall(r'review_shots\.txt -> wwe2k14_(\d+)', out)
    if len(runs) < 2:
        print(out[-2500:])
        sys.exit('review_pack: the two runs did not both report a run number (output above)')
    for line in out.splitlines():
        if 'review_shots.txt ->' in line:
            print(line[:240])
    return int(runs[0]), int(runs[1])


def most_changed(a, b, cw, ch):
    """Top-left of the cw x ch window with the largest mean abs difference (strided integral image)."""
    d = np.abs(a.astype(np.int16) - b.astype(np.int16)).max(axis=2).astype(np.float64)
    if d.max() == 0:
        return (a.shape[1] - cw) // 2, (a.shape[0] - ch) // 2
    ii = np.pad(d, ((1, 0), (1, 0))).cumsum(0).cumsum(1)
    best, pos = -1, (0, 0)
    for y in range(0, d.shape[0] - ch + 1, 20):
        for x in range(0, d.shape[1] - cw + 1, 20):
            s = ii[y + ch, x + cw] - ii[y, x + cw] - ii[y + ch, x] + ii[y, x]
            if s > best:
                best, pos = s, (x, y)
    return pos


def make_pairs(base_run, flag_run, flag, outdir):
    rows = []
    names = sorted(os.path.basename(p)[len(f'{base_run}_'):-4] for p in glob.glob(f'{CAPS}\\{base_run}_p_*.png'))
    for n in names:
        pb, pf = f'{CAPS}\\{base_run}_{n}.png', f'{CAPS}\\{flag_run}_{n}.png'
        if not os.path.exists(pf):
            continue
        ib, iff = Image.open(pb).convert('RGB'), Image.open(pf).convert('RGB')
        if iff.size != ib.size:
            iff = iff.resize(ib.size, Image.LANCZOS)
        a, b = np.asarray(ib), np.asarray(iff)
        diff = np.abs(a.astype(np.int16) - b.astype(np.int16)).max(axis=2)
        over8 = 100.0 * (diff > 8).mean()
        anyd = 100.0 * (diff > 0).mean()
        w, h = ib.size
        pair = Image.new('RGB', (2 * w + 8, h), (255, 255, 255))
        pair.paste(label(ib.copy(), 'BEFORE (base)'), (0, 0))
        pair.paste(label(iff.copy(), f'AFTER: {flag}'), (w + 8, 0))
        pair.save(f'{outdir}\\{n}_pair.png')
        cw, ch = w // 4, h // 4
        x, y = most_changed(a, b, cw, ch)
        crops = [Image.fromarray(arr[y:y + ch, x:x + cw]).resize((cw * 2, ch * 2), Image.NEAREST) for arr in (a, b)]
        sheet = Image.new('RGB', (4 * cw + 8, 2 * ch), (255, 255, 255))
        sheet.paste(label(crops[0], 'BEFORE (base)', 16), (0, 0))
        sheet.paste(label(crops[1], f'AFTER: {flag}', 16), (2 * cw + 8, 0))
        sheet.save(f'{outdir}\\{n}_crop.png')
        rows.append(dict(name=n, over8=over8, anyd=anyd, crop=(x, y, cw, ch)))
        print(f'{n}: {anyd:.2f}% of pixels differ, {over8:.2f}% by more than 8 levels; crop {x},{y},{cw}x{ch} (2x)')
    return rows


def make_clip(base_run, flag_run, flag, outdir):
    raw = os.path.join(outdir, 'clips_raw')
    cb = glob.glob(f'{raw}\\{base_run:03d}_clip_review.mp4') + glob.glob(f'{raw}\\{base_run}_clip_review.mp4')
    cf = glob.glob(f'{raw}\\{flag_run:03d}_clip_review.mp4') + glob.glob(f'{raw}\\{flag_run}_clip_review.mp4')
    if not cb or not cf:
        print('clip: raw clips missing (' + ', '.join(os.listdir(raw)) + ')')
        return None
    font = 'C\\:/Windows/Fonts/arialbd.ttf'
    ftxt = flag.replace('\\', '/').replace(':', '\\:').replace("'", '').replace('%', '')
    dt = "drawtext=fontfile='{f}':text='{t}':fontcolor=white:fontsize=26:box=1:boxcolor=black@0.8:boxborderw=8:x=10:y=10"
    fc = (f"[0:v]{dt.format(f=font, t='BEFORE (base)')}[a];[1:v]{dt.format(f=font, t='AFTER\\: ' + ftxt)}[b];"
          f"[a][b]hstack=inputs=2:shortest=1[v]")
    out = os.path.join(outdir, 'clip.mp4')
    t0 = time.time()
    r = subprocess.run(['ffmpeg', '-y', '-loglevel', 'error', '-i', cb[0], '-i', cf[0], '-filter_complex', fc, '-map', '[v]',
                        '-r', '60', '-c:v', 'libx264', '-preset', 'fast', '-crf', '18', '-pix_fmt', 'yuv420p', out],
                       capture_output=True, text=True)
    if r.returncode:
        print('clip: ffmpeg failed:', r.stderr[-800:])
        return None
    print(f'clip: {out} in {time.time() - t0:.1f} s')
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('item')
    ap.add_argument('--flag', required=True)
    ap.add_argument('--base', default='')
    ap.add_argument('--no-clip', action='store_true')
    ap.add_argument('--runs', default='', help='BASE_RUN,FLAG_RUN: reuse finished runs of the route')
    argv = sys.argv[1:]
    for i, v in enumerate(argv):  # '--flag "--x=1"': argparse would take the value for an option
        if v in ('--flag', '--base') and i + 1 < len(argv):
            argv[i] = f'{v}={argv[i + 1]}'
            argv[i + 1] = None
    a = ap.parse_args([v for v in argv if v is not None])
    outdir = f'{R}\\out\\s66\\review\\{a.item}'
    os.makedirs(outdir, exist_ok=True)
    t0 = time.time()
    if a.runs:
        base_run, flag_run = (int(x) for x in a.runs.split(','))
    else:
        base_run, flag_run = run_arms('', a.base, a.flag, outdir)
        # run_arms put the clips under outdir\clips_raw
    t_runs = time.time() - t0
    print(f'runs: base {base_run}, flag {flag_run} ({t_runs:.0f} s)')
    t1 = time.time()
    rows = make_pairs(base_run, flag_run, a.flag, outdir)
    t_pairs = time.time() - t1
    t1 = time.time()
    clip = None if a.no_clip else make_clip(base_run, flag_run, a.flag, outdir)
    t_clip = time.time() - t1
    # Session 65: perceptual scores per shot (tools\s66\flip_pair.py: NVIDIA FLIP), heat maps in outdir\flip
    flip_out = subprocess.run([sys.executable, f'{R}\\tools\\s66\\flip_pair.py', str(base_run), str(flag_run),
                               '--out', f'{outdir}\\flip'], capture_output=True, text=True).stdout
    flip_rows = {}
    for ln in flip_out.splitlines():
        p = ln.split()
        if len(p) >= 5 and p[0].startswith('p_'):
            flip_rows[p[0]] = (p[1], ' '.join(p[4:]))
    lines = [f'review_pack {a.item}', f'flag: {a.flag}', f'base extra: {a.base or "(none)"}',
             f'runs: base {base_run}, flag {flag_run}',
             f'timings: runs {t_runs:.0f} s, pictures {t_pairs:.1f} s, clip {t_clip:.1f} s']
    lines += [f'{r["name"]}: {r["anyd"]:.2f}% differ, {r["over8"]:.2f}% over 8 levels, FLIP '
              f'{flip_rows.get(r["name"], ("?", "?"))[0]} ({flip_rows.get(r["name"], ("?", "?"))[1]})' for r in rows]
    lines += [ln for ln in flip_out.splitlines() if ln.startswith('overall:')]
    open(f'{outdir}\\summary.txt', 'w').write('\n'.join(lines) + '\n')
    h = [f'<!doctype html><meta charset="utf-8"><title>Review pack: {html.escape(a.item)}</title>',
         '<style>body{font:15px Segoe UI,sans-serif;margin:20px;max-width:1400px}img,video{max-width:100%;border:1px solid #888}'
         'h2{margin-top:28px}code{background:#eee;padding:2px 5px}td,th{border:1px solid #aaa;padding:4px 10px}</style>',
         f'<h1>Review pack: {html.escape(a.item)}</h1>',
         f'<p>AFTER flag: <code>{html.escape(a.flag)}</code><br>Base: s21_lb replay, LOW/NATIVE preset'
         f'{(", " + html.escape(a.base)) if a.base else ""}. Runs: base {base_run}, flag {flag_run}. '
         'Same replay frames in both arms (replay frames are near- not byte-identical).</p>',
         '<h2>Numbers</h2><table><tr><th>check</th><th>result</th></tr><tr><td>exact (parity / hashes)</td><td>TODO</td></tr>'
         '<tr><td>gameplay identical (det)</td><td>TODO</td></tr><tr><td>A/B GPU ms, busy ms</td><td>TODO</td></tr>'
         '<tr><td>cost card (DRAM bytes, drains)</td><td>TODO</td></tr></table>']
    if clip:
        h.append('<h2>Clip: Brock\'s entrance crowd, 5 s, 60 fps</h2><video src="clip.mp4" controls loop width="1400"></video>')
    for r in rows:
        fl = flip_rows.get(r["name"], ("?", "?"))
        h.append(f'<h2>{r["name"]}</h2><p>{r["anyd"]:.2f}% of pixels differ, {r["over8"]:.2f}% by more than 8 levels. '
                 f'<b>FLIP {fl[0]}: {html.escape(fl[1])}</b> (perceptual; heat map: bright = visible).</p>'
                 f'<img src="{r["name"]}_pair.png"><br><img src="{r["name"]}_crop.png"><p>Crop {r["crop"][0]},{r["crop"][1]} '
                 f'{r["crop"][2]}x{r["crop"][3]}, shown 2x.</p><img src="flip/{r["name"]}_flip.png" width="700">')
    open(f'{outdir}\\index.html', 'w', encoding='utf-8').write('\n'.join(h))
    print(f'review pack: {outdir}\\index.html ({time.time() - t0:.0f} s total)')


if __name__ == '__main__':
    main()
