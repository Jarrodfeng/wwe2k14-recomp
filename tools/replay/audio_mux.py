"""Session 18 item 5: put the game's sound on each re-shoot clip.

  py tools/replay/audio_mux.py NAME [--wav audio.wav] [--bitrate 256k]

Reads out/reshoots/NAME/audio.wav + audio.stamps.csv (one normal-speed replay pass with --wwe2k14_audio_capture, see
src/audio_capture.h: the guest frame at each 256-sample audio frame) and every shotNN.json sidecar (first / last guest
frame of its frame-locked clip). For each shot: the audio sample of its first guest frame comes from a straight-line
fit of sample vs guest frame over the stamps around the shot (this corrects any drift between the audio clock and the
frame clock), the length is the clip's frames x 48000 / 60 samples (the 60 fps frame clock), and ffmpeg muxes it in:
shotNN_av.mp4 (video copied, AAC audio); the silent shotNN.mp4 stays. Writes shotNN_av.json (offsets, fit).
"""
import argparse
import csv
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), '..', '..'))


def ffmpeg():
    exe = shutil.which('ffmpeg')
    if not exe:
        sys.exit('ffmpeg not found on PATH')
    return exe


def fit(points):
    n = len(points)
    sx = sum(p[0] for p in points)
    sy = sum(p[1] for p in points)
    sxx = sum(p[0] * p[0] for p in points)
    sxy = sum(p[0] * p[1] for p in points)
    d = n * sxx - sx * sx
    if not d:
        return points[0][1] - 800.0 * points[0][0], 800.0
    b = (n * sxy - sx * sy) / d
    return (sy - b * sx) / n, b


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('name')
    ap.add_argument('--wav', default='audio.wav')
    ap.add_argument('--bitrate', default='256k')
    a = ap.parse_args()
    d = os.path.join(ROOT, 'out', 'reshoots', a.name)
    wav = os.path.join(d, a.wav)
    stamps_path = os.path.splitext(wav)[0] + '.stamps.csv'
    if not os.path.exists(wav) or not os.path.exists(stamps_path):
        sys.exit(f'missing {wav} or {stamps_path} (run reshoot.ps1 -Audio)')
    rows = []
    with open(stamps_path, newline='') as f:
        for r in csv.DictReader(f):
            rows.append((int(r['guest_frame']), int(r['first_sample'])))
    if not rows:
        sys.exit('no stamps')
    rate = 48000
    per_frame = rate / 60.0
    shots = sorted(x for x in os.listdir(d) if x.startswith('shot') and x.endswith('.json') and '_' not in x)
    for js in shots:
        side = json.load(open(os.path.join(d, js)))
        f0, f1, nframes = side.get('first_guest_frame'), side.get('last_guest_frame'), side.get('video_frames')
        if f0 is None or f1 is None:
            print(f'{js}: no guest frames in the sidecar; skipped')
            continue
        nframes = nframes or (f1 - f0 + 1)
        # A frame's audio starts with the first audio frame submitted during it: one point per guest frame.
        firsts = {}
        for g, s in rows:
            if f0 - 300 <= g <= f1 + 300 and g not in firsts:
                firsts[g] = s
        pts = sorted(firsts.items())
        if len(pts) < 10:
            print(f'{js}: only {len(pts)} stamped frames around {f0}..{f1}: the audio pass does not cover the shot')
            continue
        intercept, slope = fit(pts)
        start = int(round(intercept + slope * f0))
        count = int(round(nframes * per_frame))
        resid = max(abs(s - (intercept + slope * g)) for g, s in pts)
        tag = js[:-5]
        src = os.path.join(d, f'{tag}.mp4')
        out = os.path.join(d, f'{tag}_av.mp4')
        cmd = [ffmpeg(), '-y', '-loglevel', 'error', '-i', src, '-i', wav, '-filter_complex',
               f'[1:a]atrim=start_sample={max(0, start)}:end_sample={max(0, start) + count},asetpts=PTS-STARTPTS[a]',
               '-map', '0:v', '-map', '[a]', '-c:v', 'copy', '-c:a', 'aac', '-b:a', a.bitrate, '-shortest', out]
        r = subprocess.run(cmd)
        info = dict(shot=tag, first_guest_frame=f0, last_guest_frame=f1, video_frames=nframes, audio_start_sample=start,
                    audio_samples=count, samples_per_frame_fit=round(slope, 3), fit_max_residual_samples=round(resid, 1),
                    ok=r.returncode == 0, wav=os.path.basename(wav))
        json.dump(info, open(os.path.join(d, f'{tag}_av.json'), 'w'), indent=2)
        print(f'{tag}: guest frames {f0}..{f1} -> samples {start}..{start + count} ({slope:.2f} samples/frame, '
              f'fit residual <= {resid:.0f} samples = {resid / per_frame:.2f} frames) -> {out}'
              + ('' if r.returncode == 0 else '  FFMPEG FAILED'))


if __name__ == '__main__':
    main()
