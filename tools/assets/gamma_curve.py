#!/usr/bin/env python
"""Session 97: the game's front-buffer gamma table as a curve, fitted against the measured crowd curve.

The table the swap pass applies (SDK DC_LUT regs) is built by the guest: sub_82CBA908 fills a 1024-entry u16 cache
(guest 0x83947010 for type arg 0, +0x800 for 1; unconfirmed until a run shows entry 0 = 0 and entry 1023 = 1023),
sub_82CBAA90 maps the linear 256 ramp through it, sub_82CBABB0 emits it as DC_LUT writes.
So table[i] = cache[i * 1023 // 255], and the cache can be read with --wwe2k14_peek (a normal build, no SDK change).

Usage (our code, no game bytes):
  py -I tools\\assets\\gamma_curve.py probes [--base 0x83947010] [--out FILE]
      writes a live/input script (cvar wwe2k14_peek in 4 chunks of 128 words, a wait between) -> FILE
  py -I tools\\assets\\gamma_curve.py fit LOG [--base 0x83947010] [--csv OUT.csv]
      reads the [wwe2k14][peek] lines of a run log, builds cache + table, prints the curve and the fit
  py -I tools\\assets\\gamma_curve.py fit --table FILE
      FILE = 256 numbers (0..1023) separated by spaces / commas / newlines (the SDK log route)
  py -I tools\\assets\\gamma_curve.py model
      the curve the guest listing suggests (sRGB decode, then BT.709 encode), no run needed (unconfirmed constants)
"""
import argparse
import math
import re
import sys

BASE = 0x83947010
CHUNK = 128
TARGET = (1.03, 1.28)  # measured: game = 1.03 * ours ** 1.28 over 0.05-0.4 (out\runtime\s97\crowd_curve.md)


def srgb_decode(x):
    return x / 12.92 if x <= 0.04045 else ((x + 0.055) / 1.055) ** 2.4


def bt709_encode(y):
    return 4.5 * y if y < 0.018 else 1.099 * y ** 0.45 - 0.099


def model_table():
    return [min(1023, max(0, int(bt709_encode(srgb_decode(((i * 1023 // 255) / 1023.0))) * 1023.0 + 0.5))) for i in range(256)]


def probes(base, out):
    lines = ['# gamma table cache probes (gamma_curve.py probes): 512 words = 1024 u16 entries at 0x%08X' % base]
    for c in range(0, 512, CHUNK):
        spec = ','.join('g%03d=%08X' % (k, base + 4 * k) for k in range(c, c + CHUNK))
        lines += ['cvar wwe2k14_peek ' + spec, 'wait_frames 6']
    lines += ['cvar wwe2k14_peek ', 'log_state menu']
    text = '\n'.join(lines) + '\n'
    if out:
        with open(out, 'w', newline='\n') as f:
            f.write(text)
        print('wrote', out, len(lines), 'lines')
    else:
        sys.stdout.write(text)


def table_from_log(path):
    words = {}
    pat = re.compile(r'\[wwe2k14\]\[peek\] frame=\d+ g(\d{3})=0x([0-9A-Fa-f]{8})')
    with open(path, errors='replace') as f:
        for line in f:
            m = pat.search(line)
            if m:
                words[int(m.group(1))] = int(m.group(2), 16)  # the last value wins
    missing = [k for k in range(512) if k not in words]
    if missing:
        sys.exit('missing %d of 512 words (first %s): wrong base, or the probes were not all applied' % (len(missing), missing[:5]))
    cache = []
    for k in range(512):
        cache += [words[k] >> 16, words[k] & 0xFFFF]
    return cache


def fit(table, label):
    tl = [max(0, min(1023, v)) for v in table]
    print('%s: t[0]=%d t[1]=%d (the run log said 1) t[128]=%d t[255]=%d monotone=%s' % (
        label, tl[0], tl[1], tl[128], tl[255], all(b >= a for a, b in zip(tl, tl[1:]))))
    print('  in    table/1023  linear   ratio   target 1.03*x^1.28')
    for i in (4, 8, 16, 32, 48, 64, 96, 128, 160, 192, 224, 255):
        x = i / 255.0
        y = tl[i] / 1023.0
        print('  %.3f  %.4f   %.4f   %.3f   %.4f' % (x, y, x, y / x, TARGET[0] * x ** TARGET[1]))
    xs, ys = [], []
    for i in range(1, 256):
        x = i / 255.0
        if 0.05 <= x <= 0.4 and tl[i] > 0:
            xs.append(math.log(x))
            ys.append(math.log(tl[i] / 1023.0))
    n = len(xs)
    if n < 4:
        print('  too few points for a fit')
        return
    mx, my = sum(xs) / n, sum(ys) / n
    a = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sum((x - mx) ** 2 for x in xs)
    c = math.exp(my - a * mx)
    print('  log-log fit over 0.05-0.4 (%d pts): table = %.3f * x^%.3f   (measured game/ours: %.2f * x^%.2f)' % (n, c, a, *TARGET))
    worst = max(abs(tl[i] / 1023.0 - TARGET[0] * (i / 255.0) ** TARGET[1]) for i in range(13, 103))
    print('  max |table - measured curve| over 0.05-0.4: %.4f' % worst)
    print('  exponent differs from 1.28 by %+.3f: %s' % (a - TARGET[1], 'the table explains the crowd curve' if abs(a - TARGET[1]) < 0.08 else 'NOT the same curve'))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('probes')
    p.add_argument('--base', default=hex(BASE))
    p.add_argument('--out')
    f = sub.add_parser('fit')
    f.add_argument('log', nargs='?')
    f.add_argument('--base', default=hex(BASE))
    f.add_argument('--table')
    f.add_argument('--csv')
    sub.add_parser('model')
    a = ap.parse_args()
    if a.cmd == 'probes':
        probes(int(a.base, 16), a.out)
    elif a.cmd == 'model':
        fit(model_table(), 'model sRGB-decode then BT.709-encode')
    else:
        if a.table:
            with open(a.table) as fh:
                table = [int(t) for t in re.split(r'[\s,]+', fh.read().strip()) if t]
            if len(table) != 256:
                sys.exit('want 256 numbers, got %d' % len(table))
        elif a.log:
            cache = table_from_log(a.log)
            print('cache[0]=%d cache[1]=%d cache[4]=%d cache[1023]=%d' % (cache[0], cache[1], cache[4], cache[1023]))
            table = [cache[i * 1023 // 255] for i in range(256)]
        else:
            sys.exit('give LOG or --table FILE')
        fit(table, 'game table')
        mt = model_table()
        print('  vs model: max diff %d of 1023, entries differing by >2: %d' % (
            max(abs(x - y) for x, y in zip(table, mt)), sum(1 for x, y in zip(table, mt) if abs(x - y) > 2)))
        if a.csv:
            with open(a.csv, 'w') as fh:
                fh.write('in,table10\n' + ''.join('%d,%d\n' % (i, v) for i, v in enumerate(table)))
            print('wrote', a.csv)


if __name__ == '__main__':
    main()
