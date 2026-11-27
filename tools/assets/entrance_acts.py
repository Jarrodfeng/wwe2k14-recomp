r"""S98 step 6 (research, read-only): list the acts / motions of a wrestler's entrance section (evt\Entrance0N.pac).
    py -I tools\assets\entrance_acts.py CHAR [--motions LEAF]
"""
import os, re, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import spots, yanm
spots.sys.path.insert(0, os.path.join(spots.MAIN, 'tools', 'wm30'))
import pac

def main():
    char = int(sys.argv[1])
    fname, sec = spots.entrance_section(char)
    print(fname, 'leaves', len(sec))
    lay = yanm.image_layouts()
    for k in sorted(sec, key=lambda s: (len(s), s)):
        x = sec[k]
        try:
            e, _, n, _f = struct.unpack_from('>4I', x, 0)
            names = [x[o + 4:o + 36].split(b'\0')[0].decode('latin1') for o in range(n, e, 40)]
        except Exception:
            names = []
        ys = [m.start() for m in re.finditer(rb'YANM', x)]
        print(k, len(x), x[:4], 'yanm', len(ys), names[:4], '...', names[-2:])
        if '--motions' in sys.argv and k == 's/' + sys.argv[sys.argv.index('--motions') + 1]:
            nn = names[2:]
            for nm, y in zip(nn, ys):
                try:
                    tr = yanm.parse_motion(x, y, lay)
                except Exception as ex:
                    print('  ', nm, 'ERR', ex); continue
                nk = max([len(t.keys_pos) if t.keys_pos and t.keys_pos[0] != 'const' else 0 for t in tr] + [0])
                print('  ', nm, 'tracks', len(tr), 'maxposkeys', nk)
main()
