r"""Session 72: decode a combine probe screenshot (--wwe2k14_post_swap_probe=1): the right half holds rows of 32 black /
white cells (bit 31 first).
  match combine (src\post\match_combine_probe.frag): 58 rows - the 11 float constants (packed order), the four
    textures' sizes, 8 depth samples down the centre, the interpolator at one pixel.
  entrance combine (--entrance, src\post\entrance_combine_probe.frag): 68 rows - the 13 float constants, the six
    textures' sizes, the interpolator's x y z w at one pixel, the samplers' filtering.

    py -I tools\post\read_probe.py SHOT.png [--entrance]
"""
import struct
import sys

from PIL import Image


def read_words(im, rows):
    w, h = im.size
    words = []
    for row in range(rows):
        y = int((row + 0.5) * h / rows)
        word = 0
        for i in range(32):
            x = int(w / 2 + (i + 0.5) * (w / 2) / 32)
            word = (word << 1) | (1 if im.getpixel((x, y)) > 127 else 0)
        words.append(word)
    return words


def f(u):
    return struct.unpack('<f', struct.pack('<I', u))[0]


def print_constants(regs, words):
    for k, reg in enumerate(regs):
        print(f'  c{reg:<3d} = ' + '  '.join(f'{f(words[4 * k + j]):12.6g}' for j in range(4)))


def main():
    entrance = '--entrance' in sys.argv[2:]
    im = Image.open(sys.argv[1]).convert('L')
    print(f'screenshot {im.size[0]}x{im.size[1]}')
    if entrance:
        regs = [20, 21, 27, 29, 30, 31, 32, 250, 251, 252, 253, 254, 255]
        words = read_words(im, 68)
        print_constants(regs, words)
        for name, u, filt in zip(('scene (0)', 'blur A (2)', 'depth (3)', 'blur B (6)', 'bloom (7)', 'glare (8)'),
                                 words[52:58], words[62:68]):
            print(f'  {name:10s} texture {u >> 16} x {u & 0xFFFF}, filtering: {filt} of 64 points differ off-centre'
                  f' ({"point" if filt == 0 else "linear"})')
        print('  interpolator at a right-half pixel: ' + '  '.join(f'{f(u):.6g}' for u in words[58:62]))
        return
    regs = [20, 21, 27, 29, 30, 250, 251, 252, 253, 254, 255]
    words = read_words(im, 58)
    print_constants(regs, words)
    for name, u in zip(('scene', 'depth', 'far', 'near'), words[44:48]):
        print(f'  {name:5s} texture {u >> 16} x {u & 0xFFFF}')
    print('  depth at x 0.5, y = (i + 0.5) / 8: ' + '  '.join(f'{f(u):.6g}' for u in words[48:56]))
    print(f'  interpolator at a right-half pixel: x {f(words[56]):.6g}, y {f(words[57]):.6g}')


if __name__ == '__main__':
    main()
