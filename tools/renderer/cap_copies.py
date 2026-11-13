"""Session 100: lists a capture's copies (capture frame, guest frame, source and destination images with size / format, rects).
usage: py tools/renderer/cap_copies.py CAPTURE_DIR [MIN_DEST_WIDTH]   e.g. 2000 finds the 2048-wide attire builds."""
import struct, sys, pathlib
cap = pathlib.Path(sys.argv[1])
img = cap.joinpath('images.bin').read_bytes()
magic, ver, count, pad = struct.unpack_from('<IIII', img, 0)
recs = {}
off = 16
for i in range(count):
    rid, w, h, d, mips, layers, fmt, kind, usage, source, flags, gfmt = struct.unpack_from('<IHHHBBBBBBBB', img, off)
    recs[rid] = (w, h, fmt, source, flags, gfmt)
    off += 24
def name(i):
    r = recs.get(i)
    return f'img{i}' + (f'[{r[0]}x{r[1]} f{r[2]} g{r[5]} src{r[3]} fl{r[4]}]' if r else '')
want = int(sys.argv[2]) if len(sys.argv) > 2 else 0
for fp in sorted(cap.joinpath('frames').glob('frame_*.bin')):
    b = fp.read_bytes()
    _, _, cf, sc, gf, pi, fl = struct.unpack_from('<IIIIQII', b, 0)
    secs = {}
    for s in range(sc):
        sid, es, cnt, so = struct.unpack_from('<IIQQ', b, 32 + 24 * s)
        secs[sid] = (es, cnt, so)
    es, cnt, so = secs.get(5, (0, 0, 0))  # kSecCopies = 5
    for c in range(cnt):
        sid_, sm, sl, sp, did, dm, dl, dp, sx, sy, sw, sh, dx, dy, dw, dh = struct.unpack_from('<IBBHIBBHiiIIiiII', b, so + es * c)
        w = recs.get(did, (0,))[0]
        if w >= want:
            print(f'capture frame {cf} guest {gf}: copy {c} {name(sid_)} -> {name(did)} mip {dm} src {sx},{sy} {sw}x{sh} dst {dx},{dy} {dw}x{dh}')
