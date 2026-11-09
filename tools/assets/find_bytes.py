"""Session 97: find which pac leaf holds a byte pattern (e.g. bytes seen in guest memory during a live run).
Decodes each pac (tools\\wm30\\pac.py: XCTD -> EPAC -> PACH -> ZLIB / BPE leaves) and searches every decoded leaf and the
raw file. Our code; prints names and offsets only.

  py -I tools\\assets\\find_bytes.py HEX [--game DIR] [--glob "pac\\*.pac"] [--max-mb 400]
"""
import argparse
import glob
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'wm30'))
import pac  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('hex')
    ap.add_argument('--game', default=os.path.join(HERE, '..', '..', 'game_data', 'extracted'))
    ap.add_argument('--glob', action='append', default=None)
    ap.add_argument('--max-mb', type=int, default=400)
    a = ap.parse_args()
    pat = bytes.fromhex(a.hex)
    globs = a.glob or [r'pac\*.pac', r'pac\evt\*.pac', r'pac\bg\bgEtc.pac']
    files = []
    for g in globs:
        files += sorted(glob.glob(os.path.join(a.game, g)))
    for f in files:
        if os.path.getsize(f) > a.max_mb << 20:
            continue
        try:
            data = pac.load(f)
        except Exception as e:  # noqa: BLE001
            print('skip', os.path.basename(f), e)
            continue
        if pat in data:
            print('HIT', os.path.basename(f), 'top-level unpacked offset', hex(data.find(pat)))
        try:
            for name, leaf in pac.entries(data, 6):
                if pat in leaf:
                    print('HIT', os.path.basename(f), name, 'offset', hex(leaf.find(pat)), 'leaf size', len(leaf))
        except Exception as e:  # noqa: BLE001
            print('entries failed', os.path.basename(f), e)
        sys.stdout.flush()
    print('done', len(files), 'files')


if __name__ == '__main__':
    main()
