#!/usr/bin/env python3
"""S102 lane U1: the BC decoder's offline proof (0 game runs, no game files, no GPU).

  py -I tools\\bc\\bc_decode_check.py [--root DIR]

The decoder (src\\renderer\\bc_decode.h) is what makes the title card, the menus and every file texture appear on a phone with no BC formats (Mali-G615,
docs\\renderer\\TITLE_CARD_MALI.md). Pillow's DDS reader is an independent oracle: random blocks of every format are wrapped in a DDS header and decoded by Pillow and by
bc_decode_cli.exe; the two must agree.

Bars (each line PASS / FAIL):
  oracle.<fmt>    random blocks, sizes 64x64, 37x21 (edge blocks cropped), 1x1, 2x2, 4x4, 7x3: max channel difference <= 1 level (565 expansion and the 1/3 palette round
                  differently in some decoders) and at most 0.5 % of channel values differ at all.
  exact.bc3alpha  BC3 alpha / BC4 / BC5 channels (the 8-value ramps) are bit-exact (no rounding difference exists there).
  short           a source with too few blocks reports `short` (exit 2) and leaves the rest zero.
  twin.channels   the comparison itself can fail: the decoder's output with red and blue exchanged is reported as over the bar.
  twin.corrupt    one block of the input changed: the comparison reports it.
  punch           BC1 3-colour mode (c0 <= c1): index 3 decodes to transparent black (alpha 0), as the GPU's BC1_RGBA does.
"""
import argparse
import random
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import io
from PIL import Image

HERE = Path(__file__).resolve().parent
FORMATS = {1: ('BC1', b'DXT1', 8), 2: ('BC2', b'DXT3', 16), 3: ('BC3', b'DXT5', 16), 4: ('BC4', b'BC4U', 8), 5: ('BC5', b'BC5U', 16)}
results = []


def report(name, ok, evidence):
    results.append(ok)
    print(f'{"PASS" if ok else "FAIL"}  {name}: {evidence}', flush=True)


def dds(fourcc, w, h, blocks):
    header = struct.pack('<4sIIIIIII44sII4sIIIII', b'DDS ', 124, 0x1007 | 0x80000, h, w, len(blocks), 0, 1, b'\0' * 44, 32, 4, fourcc, 0, 0, 0, 0, 0)
    header += struct.pack('<IIIII', 0x1000, 0, 0, 0, 0)
    return header + blocks


def build(root, tmp):
    exe = tmp / 'bc_decode_cli.exe'
    clang = r'C:\Program Files\LLVM\bin\clang++.exe'
    p = subprocess.run([clang, '-std=c++20', '-Wall', '-Wextra', '-O1', '-D_CRT_SECURE_NO_WARNINGS', str(root / 'tools' / 'bc' / 'bc_decode_cli.cpp'), '-o', str(exe)],
                       capture_output=True, text=True)
    if p.returncode != 0:
        print(p.stdout + p.stderr)
        print('FAIL  the decoder does not compile')
        sys.exit(1)
    return exe


def ours(exe, tmp, f, w, h, blocks):
    src, dst = tmp / 'in.bin', tmp / 'out.raw'
    src.write_bytes(blocks)
    if dst.exists():
        dst.unlink()
    p = subprocess.run([str(exe), str(f), str(w), str(h), str(src), str(dst)], capture_output=True, text=True)
    return p.returncode, dst.read_bytes() if dst.exists() else b'', p.stdout


def theirs(f, w, h, blocks):
    im = Image.open(io.BytesIO(dds(FORMATS[f][1], w, h, blocks)))
    im.load()
    if f in (1, 2, 3):
        return im.convert('RGBA').tobytes(), 4
    if f == 4:
        return im.convert('L').tobytes(), 1
    rgb = im.convert('RGB').tobytes()  # BC5: R and G are the two channels
    return b''.join(rgb[i:i + 2] for i in range(0, len(rgb), 3)), 2


def diff(a, b):
    n = len(a)
    if n != len(b):
        return 999, 1.0
    worst = 0
    count = 0
    for x, y in zip(a, b):
        d = abs(x - y)
        worst = max(worst, d)
        count += d > 0
    return worst, count / max(1, n)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=str(HERE.parent.parent))
    args = ap.parse_args()
    root = Path(args.root).resolve()
    tmp = Path(tempfile.mkdtemp(prefix='bc_check_'))
    exe = build(root, tmp)
    rng = random.Random(1234)
    sizes = [(64, 64), (37, 21), (1, 1), (2, 2), (4, 4), (7, 3)]
    last = {}
    for f, (name, _fourcc, bs) in FORMATS.items():
        worst_all, frac_all, bad = 0, 0.0, []
        for (w, h) in sizes:
            nb = ((w + 3) // 4) * ((h + 3) // 4)
            blocks = bytes(rng.randrange(256) for _ in range(nb * bs))
            rc, mine, _ = ours(exe, tmp, f, w, h, blocks)
            ref, tb = theirs(f, w, h, blocks)
            if rc != 0 or len(mine) != len(ref):
                bad.append(f'{w}x{h}: exit {rc}, {len(mine)} vs {len(ref)} bytes')
                continue
            worst, frac = diff(mine, ref)
            worst_all, frac_all = max(worst_all, worst), max(frac_all, frac)
            if (w, h) == (64, 64):
                last[f] = (w, h, blocks, mine, ref)
        report(f'oracle.{name}', not bad and worst_all <= 1 and frac_all <= 0.005, f'{len(sizes)} sizes, max channel difference {worst_all}, worst share differing {frac_all:.5f} {bad}')
    # the 8-value ramps are exact
    ok = True
    ev = []
    for f in (3, 4, 5):
        w, h, blocks, mine, ref = last[f]
        if f == 3:
            m, r = mine[3::4], ref[3::4]
        else:
            m, r = mine, ref
        worst, _ = diff(m, r)
        ev.append(f'{FORMATS[f][0]} max {worst}')
        ok &= worst == 0
    report('exact.ramps', ok, ', '.join(ev))
    # short input
    rc, out, text = ours(exe, tmp, 3, 16, 16, bytes(range(16)) * 2)  # 4 blocks needed, 2 given
    rest_zero = all(out[(y * 16 + x) * 4 + k] == 0 for y in range(16) for x in range(16) for k in range(4) if x >= 8 or y >= 4)  # only blocks 0 and 1 had bytes
    report('short', rc == 2 and 'short' in text and len(out) == 16 * 16 * 4 and rest_zero, f'exit {rc}, "{text.strip()}", {len(out)} bytes written, the missing blocks zero: {rest_zero}')
    # planted faults: the comparison must be able to fail
    w, h, blocks, mine, ref = last[3]
    swapped = bytearray(mine)
    swapped[0::4], swapped[2::4] = mine[2::4], mine[0::4]
    worst, frac = diff(bytes(swapped), ref)
    report('twin.channels', worst > 1 and frac > 0.005, f'red/blue exchanged: max {worst}, share {frac:.3f}')
    bad_blocks = bytearray(blocks)
    bad_blocks[16 * 5 + 9] ^= 0xFF
    rc, other, _ = ours(exe, tmp, 3, w, h, bytes(bad_blocks))
    worst, frac = diff(other, ref)
    report('twin.corrupt', worst > 1, f'one block changed: max {worst}')
    # punch-through: c0 <= c1 and every index 3 -> alpha 0
    blk = struct.pack('<HHI', 0x1000, 0x2000, 0xFFFFFFFF)
    rc, out, _ = ours(exe, tmp, 1, 4, 4, blk)
    report('punch', rc == 0 and all(out[i] == 0 for i in range(len(out))), f'16 texels index 3: {sum(out)} (all zero expected: transparent black)')
    return 0 if all(results) else 1


if __name__ == '__main__':
    sys.exit(main())
