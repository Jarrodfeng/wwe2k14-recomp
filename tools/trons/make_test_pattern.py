"""Our own test video for custom trons (Session 17, item 1B). Nothing from the game.

Colour bars, a big frame counter, the time, and a white bar that moves across the frame once per loop, so a
screenshot shows which frame is on the tron screen and two screenshots a second apart show it advancing.

  py tools/trons/make_test_pattern.py [--out out/trons/test_pattern.mp4] [--size 1280x720] [--fps 30000/1001]
                                      [--seconds 10] [--codec h264|hevc]

Encodes with ffmpeg (found on PATH, or from the imageio-ffmpeg pip package). The video goes in out/ (gitignored).
"""
import argparse
import os
import shutil
import subprocess
import sys
from fractions import Fraction

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), '..', '..'))


def find_ffmpeg():
    exe = shutil.which('ffmpeg')
    if exe:
        return exe
    try:
        import imageio_ffmpeg
        return imageio_ffmpeg.get_ffmpeg_exe()
    except ImportError:
        sys.exit('ffmpeg not found: install it (winget install Gyan.FFmpeg) or py -m pip install imageio-ffmpeg')


def font(size):
    for name in ('consolab.ttf', 'arialbd.ttf', 'DejaVuSans-Bold.ttf'):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            pass
    return ImageFont.load_default()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=os.path.join(ROOT, 'out', 'trons', 'test_pattern.mp4'))
    ap.add_argument('--size', default='1280x720')
    ap.add_argument('--fps', default='30000/1001')
    ap.add_argument('--seconds', type=float, default=10.0)
    ap.add_argument('--codec', choices=('h264', 'hevc'), default='h264')
    a = ap.parse_args()
    w, h = (int(v) for v in a.size.lower().split('x'))
    fps = Fraction(a.fps)
    n = int(round(a.seconds * fps))
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)

    bars = [(235, 235, 235), (235, 235, 16), (16, 235, 235), (16, 235, 16), (235, 16, 235), (235, 16, 16),
            (16, 16, 235), (16, 16, 16)]
    big, small = font(h // 4), font(h // 16)
    base = Image.new('RGB', (w, h))
    d = ImageDraw.Draw(base)
    for i, c in enumerate(bars):  # top two thirds: colour bars
        d.rectangle([i * w // len(bars), 0, (i + 1) * w // len(bars) - 1, h * 2 // 3], fill=c)
    for i in range(16):  # bottom strip: grey ramp
        v = 16 + i * (235 - 16) // 15
        d.rectangle([i * w // 16, h * 2 // 3 + 1, (i + 1) * w // 16 - 1, h - 1], fill=(v, v, v))

    codec = ['-c:v', 'libx264', '-preset', 'medium', '-crf', '18'] if a.codec == 'h264' else \
            ['-c:v', 'libx265', '-preset', 'medium', '-crf', '20', '-tag:v', 'hvc1']
    cmd = [find_ffmpeg(), '-y', '-loglevel', 'error', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-s', f'{w}x{h}',
           '-r', f'{fps.numerator}/{fps.denominator}', '-i', '-', *codec, '-pix_fmt', 'yuv420p',
           '-colorspace', 'bt709', '-color_primaries', 'bt709', '-color_trc', 'bt709', '-color_range', 'tv',
           '-movflags', '+faststart', a.out]
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE)
    for f in range(n):
        im = base.copy()
        d = ImageDraw.Draw(im)
        x = f * w // n  # moving bar: one sweep per loop
        d.rectangle([x, 0, x + max(4, w // 80), h - 1], fill=(255, 255, 255))
        text = f'{f:04d}'
        box = d.textbbox((0, 0), text, font=big)
        tx, ty = (w - (box[2] - box[0])) // 2, h // 3 - (box[3] - box[1]) // 2
        d.rectangle([tx - 20, ty - 10, tx + box[2] - box[0] + 20, ty + box[3] - box[1] + 30], fill=(0, 0, 0))
        d.text((tx, ty - box[1]), text, font=big, fill=(255, 255, 255))
        d.text((20, h - h // 10), f'TEST PATTERN  {float(f / fps):6.2f} s  frame {f + 1}/{n}', font=small,
               fill=(255, 255, 0))
        p.stdin.write(im.tobytes())
    p.stdin.close()
    if p.wait():
        sys.exit(f'ffmpeg failed ({p.returncode})')
    print(f'{a.out}: {w}x{h}, {n} frames at {fps} fps ({a.codec})')


if __name__ == '__main__':
    main()
