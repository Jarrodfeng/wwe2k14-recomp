"""Turn a video with transparency into a "stacked alpha" MP4 for title reveals (Session 18, item 2).

A stacked-alpha MP4 is an ordinary H.264 video twice as tall as the picture: the colour on top, the alpha (opacity) as
grey below (white = solid, black = see-through). Any player shows both halves; the game's tron shim (src/tron_shim.h)
and the launcher read it as one picture with transparency. Name it NNN.alpha.mp4 in mods/titantron (the launcher's
Titles list does that for you). Transparency is only used where the game's own file has it (the title reveals).

  py tools/trons/make_stacked_alpha.py INPUT OUTPUT [--fps 30000/1001] [--crf 16] [--max-width 1024]

INPUT: anything ffmpeg reads with an alpha channel: ProRes 4444 (.mov), a PNG sequence ("frames/%04d.png", give
--fps), WebM with alpha (VP8 / VP9; decoded with libvpx so the alpha survives), QuickTime Animation, etc.
A video without alpha comes out fully solid (white bottom half). --max-width scales larger inputs down (default
1024: the shim's texture width for the 880x720 reveals). ffmpeg from PATH or the imageio-ffmpeg pip package.
"""
import argparse
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(__file__))
from make_test_pattern import find_ffmpeg  # noqa: E402


def probe_codec(ffmpeg, path):
    ffprobe = os.path.join(os.path.dirname(ffmpeg), 'ffprobe' + os.path.splitext(ffmpeg)[1])
    if not os.path.exists(ffprobe):
        return ''
    r = subprocess.run([ffprobe, '-v', 'error', '-select_streams', 'v:0', '-show_entries', 'stream=codec_name',
                        '-of', 'json', path], capture_output=True, text=True)
    try:
        return json.loads(r.stdout)['streams'][0]['codec_name']
    except (ValueError, KeyError, IndexError):
        return ''


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('input')
    ap.add_argument('output')
    ap.add_argument('--fps', help='frame rate for an image sequence (e.g. 30000/1001)')
    ap.add_argument('--crf', type=int, default=16)
    ap.add_argument('--max-width', type=int, default=1024)
    a = ap.parse_args()
    ffmpeg = find_ffmpeg()
    pre = []
    if '%' in a.input:  # image sequence
        pre += ['-framerate', a.fps or '30000/1001']
    else:
        codec = probe_codec(ffmpeg, a.input)
        if codec == 'vp9':
            pre += ['-c:v', 'libvpx-vp9']  # ffmpeg's own VP9 decoder drops the alpha
        elif codec == 'vp8':
            pre += ['-c:v', 'libvpx']
    w = a.max_width
    graph = (f"[0:v]format=rgba,scale='min({w},iw)':-2:flags=lanczos,format=rgba,split[c][a];"
             "[a]alphaextract,format=gray[al];"
             "[c]format=rgb24[co];"
             "[co][al]vstack=inputs=2,format=yuv420p[out]")
    out_rate = ['-r', a.fps] if a.fps and '%' not in a.input else []
    cmd = [ffmpeg, '-y', '-loglevel', 'error', *pre, '-i', a.input, '-filter_complex', graph, '-map', '[out]', '-an',
           *out_rate, '-c:v', 'libx264', '-preset', 'slow', '-crf', str(a.crf), '-colorspace', 'bt709',
           '-color_primaries', 'bt709', '-color_trc', 'bt709', '-color_range', 'tv', '-movflags', '+faststart',
           a.output]
    r = subprocess.run(cmd)
    if r.returncode:
        sys.exit(f'ffmpeg failed ({r.returncode})')
    print(f'{a.output}: stacked alpha (colour on top, alpha below)')


if __name__ == '__main__':
    main()
