r"""CAWH past-bug re-find, step 1 (S103, offline, 0 game runs): dump the mip-0 bytes of every particle image of a capture to a scratch folder, for tools\scene\f5_unit.exe --menu DIR.
  py -I tools\particles\menu_particle_dump.py CAPTURE OUTDIR          (CAPTURE: a capture folder / case zip; relative paths are looked up in the main checkout, as tex_owner.py does)
One file per particle image: NN.bin = u32 width, u32 height, u32 DDS fourcc (DXT1 / DXT3 / DXT5), then mip 0 as the capture holds it (the bytes the pac's DDS carries: tex_owner joins them by hash).
The files are scratch (game pictures): OUTDIR must be under out\ and is never committed. Exit 0 and prints the count; 1 when no particle image has a content blob."""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import tex_owner as to  # noqa: E402

# The RCTX blob's format number is the renderer's (26 for all ten menu images), not the guest's 18 / 19 / 20: the block size decides (8 bytes = DXT1; 16 = DXT3 or DXT5, written as DXT5: f5_unit tries both).


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    cap_path, out = sys.argv[1], sys.argv[2]
    got = to.collect(to.resolve(cap_path), None)
    os.makedirs(out, exist_ok=True)
    n = 0
    for im in sorted(i for i, e in got['images'].items() if 'particle' in e['kinds']):
        info = got['images'][im]['info']
        if not info:
            continue
        m0 = next(s for s in info['subs'] if s[0] == 0 and s[1] == 0)
        data = bytes(got['cap'].tex[info['off']:info['off'] + info['db']])[m0[6]:m0[6] + m0[4] * m0[5]]
        blocks = ((info['w'] + 3) // 4) * ((info['h'] + 3) // 4)
        if len(data) not in (blocks * 8, blocks * 16):
            continue
        fourcc = b'DXT1' if len(data) == blocks * 8 else b'DXT5'
        with open(os.path.join(out, '%03d.bin' % im), 'wb') as f:
            f.write(struct.pack('<III', info['w'], info['h'], struct.unpack('<I', fourcc)[0]) + data)
        n += 1
    print('menu_particle_dump: %d particle images written to %s' % (n, out))
    return 0 if n else 1


if __name__ == '__main__':
    sys.exit(main())
