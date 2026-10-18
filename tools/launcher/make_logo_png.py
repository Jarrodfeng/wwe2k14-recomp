"""Session 31: the launcher's logo picture (assets/launcher/wwe2k14_logo.png, embedded in wwe2k14_launcher.exe).

Source: Jarrod's own trace of the WWE 2K14 logo, assets/launcher/wwe2k14_logo.svg (made 2026-10-18 with
out/logo/trace_logo.py; not a game file). Input: a large transparent render of that SVG (out/logo/
wwe2k14_logo_3840x1920.png). Crops the transparent margin and scales it to 1280 px wide: the launcher scales it down
to the exact size it draws (sharp up to 1280 px on screen).

usage: py tools/launcher/make_logo_png.py [render.png] [out.png]
"""
import sys
from PIL import Image

src = sys.argv[1] if len(sys.argv) > 1 else "out/logo/wwe2k14_logo_3840x1920.png"
dst = sys.argv[2] if len(sys.argv) > 2 else "assets/launcher/wwe2k14_logo.png"
im = Image.open(src).convert("RGBA")
box = im.getchannel("A").getbbox()
pad = 8
im = im.crop((max(0, box[0] - pad), max(0, box[1] - pad), min(im.width, box[2] + pad), min(im.height, box[3] + pad)))
w = 1280
im = im.resize((w, round(im.height * w / im.width)), Image.LANCZOS)
im.save(dst, optimize=True)
print(f"{dst}: {im.width}x{im.height}")
