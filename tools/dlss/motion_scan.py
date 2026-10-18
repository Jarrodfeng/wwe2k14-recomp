"""Session 24 (DLSS): find fast motion in a frame-locked replay clip. Decodes the clip small and grey (ffmpeg), and
prints per video frame the mean absolute difference to the previous frame (whole picture), marking camera cuts
(most of the picture changes) apart from local motion. Then writes a contact sheet of the top local-motion frames.

Usage: py tools\\dlss\\motion_scan.py CLIP.mp4 FIRST_GUEST_FRAME [--from G] [--to G] [--top N] [--sheet OUT.png]
Local only (the sheet shows game frames: out\\ only).
"""
import argparse
import subprocess

import numpy as np
from PIL import Image

W, H = 320, 180


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("clip")
    ap.add_argument("first_guest", type=int)
    ap.add_argument("--from", dest="g0", type=int, default=0)
    ap.add_argument("--to", dest="g1", type=int, default=10 ** 9)
    ap.add_argument("--top", type=int, default=12)
    ap.add_argument("--sheet")
    a = ap.parse_args()
    cmd = ["ffmpeg", "-v", "error", "-i", a.clip, "-vf", f"scale={W}:{H}", "-f", "rawvideo", "-pix_fmt", "gray", "-"]
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    prev = None
    rows = []
    frames = {}
    i = 0
    while True:
        buf = p.stdout.read(W * H)
        if len(buf) < W * H:
            break
        g = a.first_guest + i
        i += 1
        cur = np.frombuffer(buf, np.uint8).astype(np.int16).reshape(H, W)
        if prev is not None and a.g0 <= g <= a.g1:
            d = np.abs(cur - prev)
            whole = float(d.mean())
            changed = float((d > 24).mean())  # share of pixels that changed a lot
            rows.append((g, whole, changed))
            frames[g] = cur.astype(np.uint8)
        prev = cur
    p.wait()
    # Cuts: over 35 % of the picture changed a lot. Local motion: the rest, ranked by the changed share.
    local = [r for r in rows if r[2] < 0.35]
    cuts = [r[0] for r in rows if r[2] >= 0.35]
    print(f"{len(rows)} frames, cuts at {cuts[:40]}")
    top = sorted(local, key=lambda r: -r[2])[: a.top]
    for g, whole, changed in sorted(top):
        print(f"guest {g}: mean diff {whole:.2f}, changed {changed * 100:.1f} %")
    if a.sheet and top:
        cols = 4
        rows_n = (len(top) + cols - 1) // cols
        sheet = Image.new("L", (cols * W, rows_n * H))
        for k, (g, _, _) in enumerate(sorted(top)):
            sheet.paste(Image.fromarray(frames[g]), ((k % cols) * W, (k // cols) * H))
        sheet.save(a.sheet)


if __name__ == "__main__":
    main()
