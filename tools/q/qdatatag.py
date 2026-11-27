r"""q cstr + the data search of q imm (S111 QCSTR): read C strings of the game image, find a 4-byte tag as DATA.

    py tools/q/q.py cstr ADDR [--n N] [--wide] [--max LEN]
    py tools/q/q.py cstr --table ADDR --count N [--stride S] [--inline [--len L]] [--wide]

  cstr ADDR            the NUL-terminated ASCII string at an image address (N consecutive strings with --n); --wide = UTF-16BE up to a 0000
                       unit. Bytes outside 32..126 print as \xNN. --max caps one string (default 256; a longer one prints "..." and is said
                       to be cut).
  --table ADDR         a table of COUNT entries STRIDE bytes apart (default 4): each entry's first word is a big-endian pointer to a string;
                       one line per entry: index, entry address, pointer, string. A pointer outside the image / to no printable text says so.
  --table --inline     the string sits IN the entry (a tag table such as the UIPR chunk tags at 0x8362D3C0, stride 8): the first L bytes (default
                       4) are the text, the rest of the entry shows as words.

  q imm VALUE | 'TAG'  (q.py cmd_imm) also answers "where is this 4-byte value as DATA in the image": a 4-char token that is not a number
                       (POF0, UIPR) or a quoted one ('BEEF') is the tag's big-endian word. data_hits() below finds every place the 4 bytes sit
                       (address, aligned or not, the ASCII), leaves out the bytes inside game functions (an instruction that happens to hold
                       them), and for each lists the data words that hold that address, and the code immediates that use it (lis + ori).
The image is the one q words reads (qwords.image_path: out/harness/index/image.bin, the main checkout's when a worktree has none): initial values.
Errors: `q cstr: ...` on stderr, exit 2.
"""
import argparse
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PLANT = os.environ.get('WWE_QCSTR_PLANT', '')     # selftest only: no_nul | keep_code switch ONE rule off so its planted twin is seen to fail
sys.path.insert(0, HERE)
import qwords  # noqa: E402

BASE = qwords.BASE
RAW_CAP = 5000          # raw 4-byte matches looked at before the scan stops (a value like 0x00010000 sits everywhere)
SHOW_HITS = 12


class CstrError(Exception):
    pass


def esc(b):
    return ''.join(chr(c) if 32 <= c < 127 else '\\x%02X' % c for c in b)


def read_cstr(data, a, wide=False, maxlen=256):
    """-> (text, cut, next_addr). ASCII bytes or UTF-16BE units up to the NUL; cut = it hit maxlen before a NUL"""
    o = a - BASE
    if o < 0 or o >= len(data):
        raise CstrError('0x%08X is outside the image (0x%08X..0x%08X)' % (a, BASE, BASE + len(data)))
    if wide:
        if o % 2:
            raise CstrError('0x%08X is not 2-byte aligned for --wide' % a)
        units = []
        p = o
        while p + 2 <= len(data) and len(units) < maxlen:
            u = (data[p] << 8) | data[p + 1]
            p += 2
            if u == 0:
                return ''.join(chr(u) if 32 <= u < 127 else '\\u%04X' % u for u in units), False, BASE + p
            units.append(u)
        return ''.join(chr(u) if 32 <= u < 127 else '\\u%04X' % u for u in units), True, BASE + p
    e = data.find(b'\0', o, o + maxlen + 1)
    if PLANT == 'no_nul':
        e = -1                  # selftest plant: read to the cap, ignore the NUL
    if e < 0:
        return esc(data[o:o + maxlen]), True, a + maxlen
    return esc(data[o:e]), False, BASE + e + 1


def table_rows(data, addr, count, stride, inline, ln, wide, maxlen=256):
    rows = []
    for i in range(count):
        ea = addr + i * stride
        o = ea - BASE
        if o < 0 or o + max(stride, 4) > len(data):
            raise CstrError('entry %d at 0x%08X is outside the image' % (i, ea))
        if inline:
            tail = ' '.join('%08X' % struct.unpack('>I', data[o + k:o + k + 4])[0] for k in range((ln + 3) // 4 * 4, stride - 3, 4))
            rows.append({'i': i, 'entry': ea, 'text': esc(data[o:o + ln]), 'tail': tail})
            continue
        p = struct.unpack('>I', data[o:o + 4])[0]
        row = {'i': i, 'entry': ea, 'ptr': p}
        if BASE <= p < BASE + len(data):
            row['text'], row['cut'], _ = read_cstr(data, p, wide, maxlen)
        rows.append(row)
    return rows


def parse_tag(s):
    """'POF0' / "'BEEF'" -> the 4 bytes' big-endian word, else None (a number, or not 4 chars)"""
    t = s.strip()
    quoted = len(t) == 6 and t[0] == t[-1] and t[0] in '\'"'
    if quoted:
        t = t[1:-1]
    if len(t) != 4 or not all(32 < ord(c) < 127 for c in t):
        return None
    if not quoted and (re.fullmatch(r'[0-9A-Fa-f]{4}', t) or t.lstrip('-').isdigit()):
        return None             # a 4-digit number or hex word is a value: write 'BEEF' in quotes for the tag
    return struct.unpack('>I', t.encode('ascii'))[0]


def data_hits(data, v, in_func=None, imm_funcs=None, cap=SHOW_HITS):
    """every place the 4 bytes of v sit in the image. -> {'n_raw', 'n_in_code', 'n_data', 'capped', 'hits': [...]}
    in_func(addr) -> a function name when addr lies inside game code (those matches are dropped from the data list, counted);
    imm_funcs(addr) -> [site text] of the code that loads that address as a constant."""
    pat = struct.pack('>I', v)
    raw, p = [], data.find(pat)
    capped = False
    while p >= 0:
        raw.append(p)
        if len(raw) >= RAW_CAP:
            capped = True
            break
        p = data.find(pat, p + 1)
    hits, n_code = [], 0
    for p in raw:
        a = BASE + p
        if in_func is not None and PLANT != 'keep_code' and in_func(a):
            n_code += 1
            continue
        hits.append(a)
    out = []
    for a in hits[:cap]:
        o = a - BASE
        ptrs, q = [], data.find(struct.pack('>I', a))
        while q >= 0 and len(ptrs) < 8:
            if q % 4 == 0:
                ptrs.append(BASE + q)
            q = data.find(struct.pack('>I', a), q + 1)
        out.append({'addr': a, 'aligned': a % 4 == 0, 'ascii': esc(data[o:o + 4]),
                    'next': struct.unpack('>I', data[o + 4:o + 8])[0] if o + 8 <= len(data) else None,
                    'pointed_by': ptrs, 'code_uses': imm_funcs(a) if imm_funcs else []})
    return {'n_raw': len(raw), 'n_in_code': n_code, 'n_data': len(hits), 'capped': capped, 'hits': out}


def print_hits(d):
    if not d['n_raw']:
        print('  as data in the image: none')
        return
    print('  as data in the image: %d place(s)%s%s' % (
        d['n_data'], ' (%d more bytes-inside-code matches left out)' % d['n_in_code'] if d['n_in_code'] else '',
        ' [scan stopped at %d raw matches]' % RAW_CAP if d['capped'] else ''))
    for h in d['hits']:
        nxt = '  next word 0x%08X' % h['next'] if h['next'] is not None else ''
        print('    0x%08X%s "%s"%s' % (h['addr'], '' if h['aligned'] else ' (unaligned)', h['ascii'], nxt))
        print('      data words pointing at it: ' + (', '.join('0x%08X' % x for x in h['pointed_by']) or 'none'))
        print('      code loading it as a constant: ' + ('; '.join(h['code_uses']) or 'none'))
    if d['n_data'] > len(d['hits']):
        print('    ... %d more' % (d['n_data'] - len(d['hits'])))


def main(argv=None, image=None):
    ap = argparse.ArgumentParser(prog='q cstr', description=__doc__.split('\n\n')[0])
    ap.add_argument('addr', nargs='?')
    ap.add_argument('--n', type=int, default=1)
    ap.add_argument('--wide', action='store_true')
    ap.add_argument('--max', dest='maxlen', type=int, default=256)
    ap.add_argument('--table', default=None, help='table address (entries of --stride bytes)')
    ap.add_argument('--count', type=int, default=None)
    ap.add_argument('--stride', type=int, default=4)
    ap.add_argument('--inline', action='store_true')
    ap.add_argument('--len', dest='ln', type=int, default=4)
    ap.add_argument('--image', default=image)
    a = ap.parse_args(sys.argv[1:] if argv is None else list(argv))
    try:
        if (a.table is None) == (a.addr is None):
            raise CstrError('give ADDR, or --table ADDR --count N [--stride S]')
        if a.n < 1 or a.maxlen < 1 or a.stride < 4 or a.ln < 1 or a.ln > a.stride:
            raise CstrError('--n >= 1, --max >= 1, --stride >= 4, 1 <= --len <= --stride')
        path = a.image or qwords.image_path()
        if not path or not os.path.exists(path):
            raise CstrError('no game image: out/harness/index/image.bin is missing (it needs a game run to remake)')
        with open(path, 'rb') as f:
            data = f.read()
        try:
            if a.table is not None:
                if a.count is None or not 1 <= a.count <= 4096:
                    raise CstrError('--table needs --count N (1..4096)')
                t = qwords.parse_addr(a.table)
                rows = table_rows(data, t, a.count, a.stride, a.inline, a.ln, a.wide, a.maxlen)
                for r in rows:
                    if a.inline:
                        print('[%d] 0x%08X: "%s"%s' % (r['i'], r['entry'], r['text'], ('  + ' + r['tail']) if r['tail'] else ''))
                    elif 'text' in r:
                        print('[%d] 0x%08X: 0x%08X -> "%s"%s' % (r['i'], r['entry'], r['ptr'], r['text'], ' ... (cut at --max)' if r['cut'] else ''))
                    else:
                        print('[%d] 0x%08X: 0x%08X -> (not an image address)' % (r['i'], r['entry'], r['ptr']))
                print('(%d entries from 0x%08X, stride %d; the image\'s initial values)' % (len(rows), t, a.stride))
                return 0
            ad = qwords.parse_addr(a.addr)
            for _ in range(a.n):
                s, cut, nxt = read_cstr(data, ad, a.wide, a.maxlen)
                print('0x%08X: "%s"%s' % (ad, s, ' ... (cut at --max %d)' % a.maxlen if cut else ''))
                ad = nxt
            return 0
        except qwords.WordsError as e:
            raise CstrError(str(e))
    except CstrError as e:
        print('q cstr: %s' % e, file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
