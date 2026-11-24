"""S106 C2: the check's one sheet: a text header (row, question, both exes + commits, verdict, the readout table) stacked over the
tools/sheets/ab_pairs.py rows of BEFORE / AFTER shots with the same labels. Game screenshots stay local (CLAUDE.md rule 1). 0 game runs.

  make_sheet(out_jpg, header_lines, before_shots, after_shots)   shots = [(label, path)]
"""
# entry: make_sheet
import os
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
AB = os.path.join(os.path.dirname(HERE), 'sheets', 'ab_pairs.py')


def header_image(lines, width=1920):
    h = 18 * len(lines) + 16
    im = Image.new('RGB', (width, h), (24, 24, 24))
    d = ImageDraw.Draw(im)
    for i, l in enumerate(lines):
        d.text((8, 8 + 18 * i), l[:260], fill=(230, 230, 230))
    return im


def make_sheet(out_jpg, header_lines, before_shots, after_shots):
    hdr = header_image(header_lines)
    pairs = []
    b = dict(before_shots)
    for lab, pa in after_shots:
        if lab in b:
            pairs += [b[lab], pa]
    body = None
    if pairs and os.path.isfile(AB):
        tmp = os.path.join(tempfile.mkdtemp(prefix='chk_sheet_'), 'rows.jpg')
        p = subprocess.run([sys.executable, '-I', AB, '--out', tmp, '--a-label', 'BEFORE', '--b-label', 'AFTER'] + pairs,
                           capture_output=True, text=True, timeout=300)
        if p.returncode == 0 and os.path.isfile(tmp):
            body = Image.open(tmp).convert('RGB')
    w = max(hdr.width, body.width if body else 0)
    im = Image.new('RGB', (w, hdr.height + (body.height if body else 0)), (24, 24, 24))
    im.paste(hdr, (0, 0))
    if body:
        im.paste(body, (0, hdr.height))
    os.makedirs(os.path.dirname(out_jpg), exist_ok=True)
    im.save(out_jpg, quality=88)
    return out_jpg
