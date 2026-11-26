r"""q words (S111 QGAPS): data words of the game image at an address, so no agent hand-reads a table with a scratch script.

    py tools/q/q.py words ADDR [N] [--as u32|u16|u8|f32|str] [--json]
    py tools/q/q.py words ADDR..ADDR2 [--as ...]       the range through ADDR2 INCLUSIVE (the word / byte / string at ADDR2 is shown)

ADDR is 0x82..., 82..., or sub_82... (8 hex digits). N = how many items (default 16; at most 4096).
  u32 (default)  one big-endian word per line: hex, decimal, and what it looks like: a pointer into the image (-> the C string there when it
                 is one, so a name table reads in one call), else a float when the bits are a plausible f32
  f32            one float per line (also the hex bits)
  u16 / u8       rows of 8 / 16 values
  str            N consecutive NUL-terminated strings from ADDR, each with its address
The image is out/harness/index/image.bin (the game as loaded, base 0x82000000; the main checkout's copy when a worktree has none): the INITIAL
value of a data word, which the game may change at run time. Nothing from the image is written anywhere. Errors: `q words: ...` on stderr, exit 2.
"""
import argparse
import json
import math
import os
import re
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
BASE = 0x82000000
MAX_N = 4096
PLANT = os.environ.get('WWE_QWORDS_PLANT', '')     # selftest only: str_any | f32_any switch ONE rule off so its planted twin is seen to fail
KINDS = ('u32', 'u16', 'u8', 'f32', 'str')
SIZE = {'u32': 4, 'f32': 4, 'u16': 2, 'u8': 1}


class WordsError(Exception):
    pass


def _main_checkout(root):
    try:
        out = subprocess.run(['git', '-C', root, 'rev-parse', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if out:
            cd = os.path.abspath(os.path.join(root, out))
            if os.path.basename(cd) == '.git':
                return os.path.dirname(cd)
    except (OSError, subprocess.SubprocessError):
        pass
    return os.path.abspath(root)


def image_path(root=None):
    """this checkout's image.bin, else the main checkout's; None when neither exists"""
    root = root or ROOT
    for r in (root, _main_checkout(root)):
        p = os.path.join(r, 'out', 'harness', 'index', 'image.bin')
        if os.path.exists(p):
            return p
    return None


def parse_addr(s):
    m = re.fullmatch(r'(?:sub_|0[xX])?([0-9A-Fa-f]{8})', s.strip())
    if not m:
        raise WordsError(f'{s}: not an address (8 hex digits: 0x82..., 82..., sub_82...)')
    return int(m.group(1), 16)


def cstr_at(data, a, maxlen=48):
    """the printable C string at image address a (>= 3 chars, NUL-ended within maxlen), else None"""
    o = a - BASE
    if o < 0 or o >= len(data):
        return None
    e = data.find(b'\0', o, o + maxlen + 1)
    if e < 0 or e - o < 3:
        return None
    b = data[o:e]
    if PLANT == 'str_any' or all(32 <= c < 127 for c in b):     # selftest plant: the printable rule off
        return b.decode('ascii')
    return None


def f32_of(v):
    f = struct.unpack('>f', struct.pack('>I', v))[0]
    if math.isfinite(f) and f != 0 and (PLANT == 'f32_any' or 1e-6 <= abs(f) <= 1e9):     # selftest plant: the range rule off
        return f
    return None


def read_words(data, addr, n, kind, stop=None):
    """-> [{'addr', 'value', ...}] for n items of `kind`; WordsError past the end of the image. stop (--as str, ADDR..ADDR2): no string starts after it"""
    sz = SIZE.get(kind)
    rows = []
    if kind == 'str':
        a = addr
        for _ in range(n):
            if stop is not None and a > stop:
                break
            o = a - BASE
            if o < 0 or o >= len(data):
                raise WordsError(f'0x{a:08X} is outside the image (0x{BASE:08X}..0x{BASE + len(data):08X})')
            e = data.find(b'\0', o)
            e = len(data) if e < 0 else e
            rows.append({'addr': a, 'str': data[o:e].decode('latin-1')})
            a += e - o + 1
        return rows
    if addr % sz:
        raise WordsError(f'0x{addr:08X} is not {sz}-byte aligned for --as {kind}')
    o0 = addr - BASE
    if o0 < 0 or o0 + sz * n > len(data):
        raise WordsError(f'0x{addr:08X} + {n} x {sz} bytes is outside the image (0x{BASE:08X}..0x{BASE + len(data):08X})')
    for i in range(n):
        o = o0 + sz * i
        if kind in ('u32', 'f32'):
            v = struct.unpack('>I', data[o:o + 4])[0]
            row = {'addr': addr + 4 * i, 'value': v}
            if BASE <= v < BASE + len(data) and kind == 'u32':
                row['ptr'] = True
                s = cstr_at(data, v)
                if s is not None:
                    row['str'] = s
            f = f32_of(v) if not row.get('ptr') else None
            if f is not None or kind == 'f32':
                row['f32'] = struct.unpack('>f', struct.pack('>I', v))[0]
        else:
            v = int.from_bytes(data[o:o + sz], 'big')
            row = {'addr': addr + sz * i, 'value': v}
        rows.append(row)
    return rows


def fmt(row, kind):
    a = f'0x{row["addr"]:08X}'
    if kind == 'str':
        return f'{a}: "{row["str"]}"'
    v = row['value']
    if kind == 'f32':
        return f'{a}: {row["f32"]:.9g}  (0x{v:08X})'
    s = f'{a}: 0x{v:08X}  {v}'
    if row.get('ptr'):
        s += f'  -> 0x{v:08X}' + (f' "{row["str"]}"' if 'str' in row else '')
    elif 'f32' in row:
        s += f'  f32 {row["f32"]:.9g}'
    return s


def main(argv=None, image=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    ap = argparse.ArgumentParser(prog='q words', description=__doc__.split('\n\n')[0])
    ap.add_argument('addr')
    ap.add_argument('n', nargs='?', type=int, default=None)
    ap.add_argument('--as', dest='kind', choices=KINDS, default='u32')
    ap.add_argument('--image', default=image, help='another image file (selftest)')
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args(argv)
    try:
        kind = a.kind
        stop = None
        if '..' in a.addr:
            lo_s, hi_s = a.addr.split('..', 1)
            lo, hi = parse_addr(lo_s), parse_addr(hi_s)
            if hi < lo:
                raise WordsError('ADDR..ADDR2: ADDR2 must not be below ADDR')
            n = (hi - lo) // SIZE.get(kind, 1) + 1 if kind != 'str' else (a.n or MAX_N)
            stop = hi if kind == 'str' else None      # review S111: the strings through ADDR2, not N (4096) strings from ADDR
        else:
            lo = parse_addr(a.addr)
            n = a.n if a.n is not None else 16
        if n < 1 or n > MAX_N:
            raise WordsError(f'N must be 1..{MAX_N}')
        path = a.image or image_path()
        if not path or not os.path.exists(path):
            raise WordsError('no game image: out/harness/index/image.bin is missing (it needs a game run to remake)')
        with open(path, 'rb') as f:
            data = f.read()
        rows = read_words(data, lo, n, kind, stop)
    except WordsError as e:
        print(f'q words: {e}', file=sys.stderr)
        return 2
    if a.json:
        print(json.dumps({'addr': f'0x{lo:08X}', 'as': kind, 'rows': rows}, indent=1))
        return 0
    if kind in ('u16', 'u8'):
        per = 8 if kind == 'u16' else 16
        w = SIZE[kind] * 2
        for i in range(0, len(rows), per):
            chunk = rows[i:i + per]
            print(f'0x{chunk[0]["addr"]:08X}: ' + ' '.join(f'{r["value"]:0{w}X}' for r in chunk))
    else:
        for r in rows:
            print(fmt(r, kind))
    print(f'({len(rows)} {kind} from 0x{lo:08X}; the image\'s initial value, the game may change it at run time)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
