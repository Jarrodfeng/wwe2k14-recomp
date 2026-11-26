"""Selftest for q words (S111 QGAPS). 0 game runs: a PLANTED image (never the game's). Each check has a planted twin the answer must tell apart.
The real-image line (q words 0x8218B8CC) runs only when out/harness/index/image.bin exists, else SKIP. Exit 1 = a check failed."""
import contextlib
import io
import os
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qwords  # noqa: E402

fails = []


def check(what, ok, detail=''):
    print('%s %s%s' % ('ok  ' if ok else 'FAIL', what, '' if ok else '  ' + str(detail)))
    if not ok:
        fails.append(what)


def run(img, argv):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        rc = qwords.main(argv + ['--image', img])
    return rc, out.getvalue(), err.getvalue()


def main():
    base = qwords.BASE
    words = [0x3F800000,            # +0   1.0
             base + 0x40,           # +4   pointer to the string "g_fPlanted"
             0,                     # +8   zero
             0xC0000000,            # +C   -2.0
             base + 0x50,           # +10  pointer to bytes that are not a string (twin of +4)
             0x0BAD0000,            # +14  outside the image, not a float in range either
             0x0000012C,            # +18  300 (twin of the float 1.0: 0x12C as f32 is 4e-43, not shown as a float)
             0x41200000]            # +1C  10.0
    blob = bytearray(0x80)
    for i, w in enumerate(words):
        struct.pack_into('>I', blob, 4 * i, w)
    blob[0x40:0x40 + 11] = b'g_fPlanted\0'
    blob[0x50:0x54] = b'\x01\x02\x03\x00'
    blob[0x60:0x60 + 6] = b'first\0'
    blob[0x66:0x66 + 7] = b'second\0'
    d = tempfile.mkdtemp(prefix='qwords_')
    img = os.path.join(d, 'image.bin')
    with open(img, 'wb') as f:
        f.write(bytes(blob))
    try:
        rc, out, _ = run(img, ['0x82000000', '8'])
        lines = out.splitlines()
        check('u32: 8 rows + the closing line', rc == 0 and len(lines) == 9, out)
        check('u32: 1.0 shows its float', 'f32 1' in lines[0] and '0x3F800000' in lines[0], lines[0])
        check('u32: a pointer to a C string names the string', '"g_fPlanted"' in lines[1], lines[1])
        check('twin: a pointer to non-text bytes names no string', '->' in lines[4] and '"' not in lines[4], lines[4])
        check('twin: a word outside the image is no pointer', '->' not in lines[5], lines[5])
        check('twin: 300 is an integer, not a tiny float', 'f32' not in lines[6] and ' 300' in lines[6], lines[6])
        check('u32: zero has no float note', 'f32' not in lines[2], lines[2])
        rc, out, _ = run(img, ['82000000..8200000C'])
        check('A..B is inclusive of the word at B', len([l for l in out.splitlines() if l.startswith('0x8')]) == 4, out)
        rc, out, _ = run(img, ['0x82000000', '2', '--as', 'f32'])
        check('f32: 1.0 then 0.0 no, the pointer bits as a float', out.splitlines()[0].startswith('0x82000000: 1 ') , out)
        rc, out, _ = run(img, ['0x82000060', '2', '--as', 'str'])
        check('str: two consecutive strings with their addresses', '0x82000060: "first"' in out and '0x82000066: "second"' in out, out)
        rc, out, err = run(img, ['82000060..82000060', '--as', 'str'])
        check('str A..B stops at B (the string at B is the last; review: it read 4096 strings and ran off the image)',
              rc == 0 and '"first"' in out and '"second"' not in out, (rc, out, err))
        rc, out, err = run(img, ['82000060..82000066', '--as', 'str'])
        check('twin: str A..B with B at the second string shows both', rc == 0 and '"first"' in out and '"second"' in out and len([l for l in out.splitlines() if l.startswith('0x8200')]) == 2, (rc, out, err))
        rc, out, _ = run(img, ['0x82000000', '4', '--as', 'u8'])
        check('u8: one row of 4 bytes', '3F 80 00 00' in out, out)
        rc, out, _ = run(img, ['0x82000001', '1'])
        check('unaligned u32 is refused', rc == 2, (rc, out))
        rc, out, err = run(img, ['0x82000078', '16'])
        check('past the end of the image is refused, not clamped', rc == 2 and 'outside the image' in err, (rc, err))
        rc, out, err = run(img, ['0x7FFFFFF0'])
        check('an address below the image is refused', rc == 2, (rc, err))
    finally:
        try:
            os.remove(img)
            os.rmdir(d)
        except OSError:
            pass
    real = qwords.image_path()
    if real:
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = qwords.main(['0x8218B8C0', '4', '--as', 'f32'], image=real)
        check('real image: 0x8218B8C0 reads (the depth range 1.0 the seam rows cite)', rc == 0 and out.getvalue().splitlines()[0].startswith('0x8218B8C0: 1 '), out.getvalue()[:200])
    else:
        print('SKIP real image: no out/harness/index/image.bin')
    print('FAILED: %s' % fails if fails else 'all passed')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
