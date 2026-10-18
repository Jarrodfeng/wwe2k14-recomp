"""Turns a PNG into a multi-size Windows .ico (16 .. 256 px) for the exe icons (CMakeLists.txt, Session 17).

    py tools/icon/make_ico.py game_logo.png game_logo.ico      (re-run after changing the PNG; commit both)

game_logo.png / game_logo.ico (repo root): a fan-made SteamGridDB icon, committed by Jarrod's decision. The build only
reads the .ico (no Python needed to build).
"""
import sys

from PIL import Image

src, dst = sys.argv[1], sys.argv[2]
im = Image.open(src).convert('RGBA')
side = max(im.size)
if im.size[0] != im.size[1]:  # pad to a square, centred
    sq = Image.new('RGBA', (side, side), (0, 0, 0, 0))
    sq.paste(im, ((side - im.size[0]) // 2, (side - im.size[1]) // 2))
    im = sq
im.save(dst, format='ICO', sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
print('wrote', dst)
