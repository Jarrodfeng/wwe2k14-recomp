"""Compare two guest-memory snapshots from --wwe2k14_scenario_observe (logs/scenario_NNN_KK_<point>_<M|G|A>.bin,
src/scenario.h), or list the non-zero words of one. Prints offsets and big-endian word values only.

    py tools\\scenarios\\diff_bin.py OLD.bin NEW.bin [--layout M|G|A] [--max N]
    py tools\\scenarios\\diff_bin.py ONE.bin --nonzero [--layout M|G|A] [--max N]

Layouts (docs/systems/match_setup.md): M = 724-byte header + 43 participant entries of 3,580 bytes ("entry i +off");
A = 43 slots of 56 bytes ("slot i +off"); G = flat offsets. Default: taken from the file name's last letter.
Runs of consecutive changed words are grouped on one line.
"""
import argparse
import os
import struct
import sys


def region(layout, off):
    if layout == 'M':
        if off < 724:
            return 'hdr +%d' % off
        i, r = divmod(off - 724, 3580)
        return 'entry %d +%d' % (i, r)
    if layout == 'A':
        i, r = divmod(off, 56)
        return 'slot %d +%d' % (i, r)
    return 'G +%d' % off


def words(data):
    n = len(data) // 4
    return struct.unpack('>%dI' % n, data[:n * 4])


def groups(offsets):
    run = []
    for off in offsets:
        if run and off != run[-1] + 4:
            yield run
            run = []
        run.append(off)
    if run:
        yield run


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('files', nargs='+')
    ap.add_argument('--layout', choices=['M', 'G', 'A'])
    ap.add_argument('--nonzero', action='store_true')
    ap.add_argument('--max', type=int, default=400, help='max lines (default 400)')
    a = ap.parse_args()
    layout = a.layout or os.path.splitext(a.files[0])[0][-1:]
    if layout not in ('M', 'G', 'A'):
        layout = 'G'
    if a.nonzero:
        if len(a.files) != 1:
            ap.error('--nonzero takes one file')
        w = words(open(a.files[0], 'rb').read())
        offs = [i * 4 for i, v in enumerate(w) if v]
        old = None
    else:
        if len(a.files) != 2:
            ap.error('give OLD.bin NEW.bin')
        wa = words(open(a.files[0], 'rb').read())
        wb = words(open(a.files[1], 'rb').read())
        if len(wa) != len(wb):
            print('sizes differ: %d vs %d words (comparing the common part)' % (len(wa), len(wb)))
        n = min(len(wa), len(wb))
        offs = [i * 4 for i in range(n) if wa[i] != wb[i]]
        old, w = wa, wb
    print('%d words %s (layout %s)' % (len(offs), 'non-zero' if a.nonzero else 'changed', layout))
    lines = 0
    for run in groups(offs):
        if lines >= a.max:
            print('... (more; raise --max)')
            break
        vals = ' '.join('%08X' % w[o // 4] for o in run[:8])
        more = ' ...' if len(run) > 8 else ''
        if old is None:
            print('%-18s %3d words: %s%s' % (region(layout, run[0]), len(run), vals, more))
        else:
            olds = ' '.join('%08X' % old[o // 4] for o in run[:8])
            print('%-18s %3d words: %s%s  (was %s%s)' % (region(layout, run[0]), len(run), vals, more, olds, more))
        lines += 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
