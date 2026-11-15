r"""S102 lane L1 (review F6): a slowed side-by-side clip of the baked cube against the per-pixel lights on a moving shot, and a frame-to-frame measure (0 game runs: fview headless + ffmpeg).
  py -I tools\l1\l1_clip.py [--cap bryan] [--frames 0:91] [--out DIR] [--fps 6] [--window 640x480]
Panels, every one labelled: OFF (the baked cube) | ON (four lights per pixel) | difference x 8. The window follows the wrestlers' silhouette (the centre of the pixels a --draw-drop of the wrestlers changes,
smoothed over 9 frames) so a moving camera keeps the wrestler in view. The clip plays at --fps (the capture is 30 frames a second, so 6 is five times slower). PNG frames, then ffmpeg (libx264,
yuv420p, CRF 14). Also prints the frame-to-frame terminator measure on the wrestler pixels: the mean absolute change of each arm from one frame to the next, and the mean absolute change of the DIFFERENCE
(on - off) from one frame to the next: if the analytic path flickered or crawled, the difference would change frame to frame by more than the pictures themselves do."""
import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import numpy as np  # noqa: E402
from PIL import Image, ImageDraw  # noqa: E402
import l1_pics  # noqa: E402

FFMPEG = shutil.which('ffmpeg') or r'C:\Users\Jarrod\AppData\Local\Microsoft\WinGet\Packages\Gyan.FFmpeg_Microsoft.Winget.Source_8wekyb3d8bbwe\ffmpeg-8.0-full_build\bin\ffmpeg.exe'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cap', default='bryan')
    ap.add_argument('--frames', default='0:91')
    ap.add_argument('--out', default='')
    ap.add_argument('--fps', type=int, default=6)
    ap.add_argument('--window', default='640x480')
    a = ap.parse_args()
    root = os.path.normpath(os.path.join(HERE, '..', '..'))
    main_dir = l1_pics.main_checkout(root)
    fview = os.path.join(root, 'out', 'renderer', 'dev', 'fview.exe')
    cap = os.path.join(main_dir, 'out', 'lane86', a.cap, 'capture')
    lo, hi = [int(x) for x in a.frames.split(':')]
    frames = list(range(lo, hi))
    ww, wh = [int(x) for x in a.window.split('x')]
    out_dir = a.out or os.path.join(main_dir, 'out', 's102', 'sheets', 'l1')
    os.makedirs(out_dir, exist_ok=True)
    work = os.path.join(root, 'out', 'l1', 'clip_' + a.cap)
    base = ['--copy-batch', 'on']
    off, _ = l1_pics.render_arm(fview, cap, frames, os.path.join(work, 'off'), base)
    on, text = l1_pics.render_arm(fview, cap, frames, os.path.join(work, 'on'), base + ['--prelight-analytic'])
    drop, _ = l1_pics.render_arm(fview, cap, frames, os.path.join(work, 'drop'), base + ['--draw-drop', '@class=wrestlers'])
    masks = {f: np.abs(off[f] - drop[f]).max(axis=2) > 0 for f in frames}
    # the window centre: the centroid of the wrestler pixels, smoothed over 9 frames
    cent = []
    for f in frames:
        ys, xs = np.nonzero(masks[f])
        cent.append((xs.mean(), ys.mean()) if xs.size else (cent[-1] if cent else (off[f].shape[1] / 2, off[f].shape[0] / 2)))
    cent = np.array(cent)
    sm = np.stack([np.convolve(np.pad(cent[:, k], 4, mode='edge'), np.ones(9) / 9, mode='valid') for k in range(2)], axis=1)
    frames_dir = os.path.join(work, 'clip_frames')
    shutil.rmtree(frames_dir, ignore_errors=True)
    os.makedirs(frames_dir)
    H, W = off[frames[0]].shape[:2]
    for n, f in enumerate(frames):
        x0 = int(min(max(0, sm[n, 0] - ww / 2), W - ww))
        y0 = int(min(max(0, sm[n, 1] - wh / 2), H - wh))
        crop = lambda im: im[y0:y0 + wh, x0:x0 + ww].astype(np.uint8)
        diff = np.clip(np.abs(off[f] - on[f])[y0:y0 + wh, x0:x0 + ww] * 8, 0, 255).astype(np.uint8)
        sheet = Image.new('RGB', (3 * ww + 20, wh + 44), (24, 24, 24))
        d = ImageDraw.Draw(sheet)
        for i, (title, im) in enumerate((('OFF: the baked cube (the game\'s lighting)', crop(off[f])), ('ON: the four lights evaluated per pixel', crop(on[f])), ('difference x 8 (black = identical)', diff))):
            sheet.paste(Image.fromarray(im), (i * (ww + 10), 22))
            d.text((i * (ww + 10) + 4, 5), title, fill=(255, 230, 140))
        d.text((6, wh + 28), '%s frame %d of %d (slowed %.1fx: the capture is 30 frames a second, this plays at %d)' % (a.cap, f, hi - 1, 30.0 / a.fps, a.fps), fill=(220, 220, 220))
        sheet.save(os.path.join(frames_dir, 'f_%04d.png' % n))
    mp4 = os.path.join(out_dir, 'l1_prelight_%s_slowed_side_by_side.mp4' % a.cap)
    subprocess.run([FFMPEG, '-y', '-loglevel', 'error', '-framerate', str(a.fps), '-i', os.path.join(frames_dir, 'f_%04d.png'), '-c:v', 'libx264', '-pix_fmt', 'yuv420p', '-crf', '14', mp4], check=True)
    print('clip: %s (%d frames, %.1f s)' % (mp4, len(frames), len(frames) / a.fps))
    # the frame-to-frame terminator measure on the wrestler pixels
    d_off, d_on, d_dif = [], [], []
    for p, q in zip(frames[:-1], frames[1:]):
        m = masks[p] & masks[q]
        if m.sum() < 1000:
            continue
        d_off.append(np.abs(off[q] - off[p]).mean(axis=2)[m].mean())
        d_on.append(np.abs(on[q] - on[p]).mean(axis=2)[m].mean())
        d_dif.append(np.abs((on[q] - off[q]) - (on[p] - off[p])).mean(axis=2)[m].mean())
    print('frame-to-frame, wrestler pixels (mean abs change of 255, per channel average), %d pairs: OFF %.3f, ON %.3f, the difference (on - off) %.3f' % (len(d_off), np.mean(d_off), np.mean(d_on), np.mean(d_dif)))
    print('the on - off difference changes from frame to frame by %.1f%% of what the picture itself changes: %s' % (100 * np.mean(d_dif) / max(1e-9, np.mean(d_off)),
          'no flicker or crawl added' if np.mean(d_dif) < 0.25 * np.mean(d_off) else 'CHECK: the analytic path adds frame-to-frame change'))
    return 0 if np.mean(d_dif) < 0.25 * np.mean(d_off) else 1


if __name__ == '__main__':
    sys.exit(main())
