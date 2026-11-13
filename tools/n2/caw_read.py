"""S101 lane 118 (N2 / L-6): the Python reference reader of the Create a Superstar editor's drawn state. 0 game runs.

read_cstate(frame_path, geom_bytes, opts=None) -> dict   (the SAME fields and rules as src/caw_text.h ReadCawState; the gate compares the two)
    guest        guest frame of the file
    has_text     the file holds run strings (names.h section 69, count > 0); False = "no text", never a guess
    seq          the sequence id read from the text draws' widget strings ("screen seq=76 menu=-1"), -1 = none read
    seq76        at least one widget read and every one says 76
    title        the run at (140, 58), "" if none
    rows         the runs at x 144, y = 118 + 24 k, list indexed by k ("" where the row k is not drawn)
    complete     the rows are k = 0..n-1 with no gap, n >= 1
    hot          the index of the one hot row, -1 none, -2 more than one
    hot_n        how many rows are hot;  hot_r the hot row's red (0 if none)
    row_r, row_g, row_b   each row's colour (VS bank register 1), 3 decimals; the driver tells grey and amber rows by them
    tab          the index (0-3) of the red dot of the four 16 px T_Sprite dots at x 362 + 16 k, -1 none drawn / none red, -2 more than one red
    dots         how many of the four dots are drawn
The rules are the measured ones of docs/research/s101/N2_EDITOR_DRIVER.md section 1. `opts` are the planted twins' knobs only (the shipped
defaults are CawOpts()):  bank 'vs'|'ps', min_r, ratio, white (all colours read as 1,1,1), strip_runs (read the file as if section 69 were absent).
"""
import os
import re
import struct
import sys
from dataclasses import dataclass

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from caw_text_read import load, get_name, expand_banks

TITLE_X, TITLE_Y = 140.0, 58.0
ROW_X, ROW_Y0, ROW_STEP = 144.0, 118.0, 24.0
DOT_X0, DOT_STEP, DOT_Y = 362.0, 16.0, 66.0
KIND_SPRITE = 3
SEQ_RE = re.compile(r'screen seq=(\d+)')
DRAW_FMT = '<QQQIIIIIIIIQQIIII'


@dataclass
class CawOpts:
    bank: str = 'vs'
    min_r: float = 0.05   # the review's floor: the pulse goes to 0.165 on the kept walk; g and b <= 0.25 r separates red from the greys
    ratio: float = 0.25
    white: bool = False
    strip_runs: bool = False


def is_hot(c, opts=CawOpts()):
    """r clearly above g and b, g and b small (N2_EDITOR_DRIVER.md section 1; S101 lane 118 review: r >= 0.05, g and b <= 0.25 r)."""
    return c[0] >= opts.min_r and c[1] <= opts.ratio * c[0] and c[2] <= opts.ratio * c[0]


def is_red_dot(c):
    return c[0] >= 0.6 and c[1] <= 0.25 * c[0] and c[2] <= 0.25 * c[0]


def _near(a, b, e=0.6):
    return abs(a - b) <= e


def read_cstate(path, geom, opts=None):
    opts = opts or CawOpts()
    b, gf, secs = load(path)
    out = dict(guest=gf, has_text=False, seq=-1, seq76=False, title='', rows=[], row_r=[], row_g=[], row_b=[], complete=False, hot=-1, hot_n=0, hot_r=0.0, tab=-1, dots=0)
    if 67 not in secs or 69 not in secs or opts.strip_runs or secs[69][1] == 0:
        return out
    es, cnt, o = secs[67]
    S = b[o:o + cnt * es]
    es, cnt, o = secs[69]
    runs = [struct.unpack_from('<IIII', b, o + i * es) for i in range(cnt)]
    out['has_text'] = True
    des, dc, do = secs[4]
    lay = [struct.unpack_from('<IIII', b, secs[11][2] + i * secs[11][0]) for i in range(secs[11][1])]
    banks = expand_banks(b, secs)
    org = []
    if 64 in secs:
        es, cnt, o = secs[64]
        org = [struct.unpack_from('<BBH7I', b, o + i * es) for i in range(cnt)]

    def draw(d):
        return struct.unpack_from(DRAW_FMT, b, do + d * des)

    def colour(t):
        if opts.white:
            return (1.0, 1.0, 1.0, 1.0)
        bank = t[7] if opts.bank == 'vs' else t[8]
        return struct.unpack_from('<4f', banks[bank], 16)

    seqs = []
    items = []      # (draw, text, x, y, colour)
    for d, s, u, f in runs:
        if d >= dc:
            continue
        t = draw(d)
        vo, lo = t[11], t[10]
        if lo == 0xFFFFFFFF or not t[15]:
            continue
        x, y = struct.unpack_from('<2f', geom, vo)
        items.append((d, get_name(S, s) or '', round(x, 1), round(y, 1), colour(t)))
        if d < len(org) and org[d][9]:
            m = SEQ_RE.match(get_name(S, org[d][9]) or '')
            if m:
                seqs.append(int(m.group(1)))
    if seqs:
        out['seq'] = seqs[0]
        out['seq76'] = all(v == 76 for v in seqs)
    # title and rows: the first draw of each (x, y) place, the shadow repeat (another x, y) is another place
    seen = set()
    rows = {}
    offgrid = False   # a run at x 144 in the list's y range that is on no row slot (a row sliding in)
    for d, t, x, y, c in items:
        if _near(x, TITLE_X) and _near(y, TITLE_Y) and not out['title']:
            out['title'] = t
        if _near(x, ROW_X) and ROW_Y0 - 0.5 <= y <= 580.0:
            k = round((y - ROW_Y0) / ROW_STEP)
            if not (abs(y - (ROW_Y0 + ROW_STEP * k)) <= 0.6 and k >= 0):
                offgrid = True
            if abs(y - (ROW_Y0 + ROW_STEP * k)) <= 0.6 and k >= 0 and k not in rows:
                rows[k] = (t, c)
    n = (max(rows) + 1) if rows else 0
    out['rows'] = [rows[k][0] if k in rows else '' for k in range(n)]
    out['row_r'] = [round(rows[k][1][0], 3) if k in rows else 0.0 for k in range(n)]
    out['row_g'] = [round(rows[k][1][1], 3) if k in rows else 0.0 for k in range(n)]
    out['row_b'] = [round(rows[k][1][2], 3) if k in rows else 0.0 for k in range(n)]
    out['complete'] = n >= 1 and not offgrid and all(k in rows for k in range(n))
    hots = [k for k in rows if is_hot(rows[k][1], opts)]
    out['hot_n'] = len(hots)
    out['hot'] = hots[0] if len(hots) == 1 else (-2 if hots else -1)
    out['hot_r'] = round(rows[hots[0]][1][0], 3) if len(hots) == 1 else 0.0
    # the tab dots: sprite-class draws, the first quad at y 64-68 and x 362 + 16 k
    dots = {}
    for d in range(min(dc, len(org))):
        if org[d][0] != KIND_SPRITE:
            continue
        t = draw(d)
        if t[10] == 0xFFFFFFFF or not t[15] or not t[6]:
            continue
        x, y = struct.unpack_from('<2f', geom, t[11])
        k = round((x - DOT_X0) / DOT_STEP)
        if 0 <= k <= 3 and _near(x, DOT_X0 + DOT_STEP * k, 0.6) and DOT_Y - 2 <= y <= DOT_Y + 2:
            dots.setdefault(k, colour(t))
    out['dots'] = len(dots)
    red = [k for k, c in dots.items() if is_red_dot(c)]
    out['tab'] = red[0] if len(red) == 1 else (-2 if red else -1)
    return out


def summary(s):
    """One line for diffs and logs."""
    return 'seq%s title=%r n=%d%s hot=%d tab=%d dots=%d' % (s['seq'], s['title'], len(s['rows']), '' if s['complete'] else '(gap)', s['hot'], s['tab'], s['dots'])


def flash_between(prev, cur):
    """The hot row changed, or its red jumped up by more than 0.1, between two states (src/caw_text.h Flash)."""
    return cur['hot'] >= 0 and prev['hot'] >= 0 and (cur['hot'] != prev['hot'] or cur['hot_r'] - prev['hot_r'] > 0.1)


def line(n, s, flash):
    """The text form src/caw_text.h Line() prints (plus the capture index and the flash), for the gate's frame-by-frame comparison."""
    out = '%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%.3f\t%d\t%d\t%s' % (
        n, s['guest'], 1 if s['has_text'] else 0, s['seq'], 1 if s['seq76'] else 0, 1 if s['complete'] else 0, s['hot'], s['hot_n'], s['tab'],
        s['hot_r'], s['dots'], len(s['rows']), s['title'])
    for t, r, g, b in zip(s['rows'], s['row_r'], s['row_g'], s['row_b']):
        out += '\t%.3f,%.3f,%.3f\t%s' % (r, g, b, t)
    return out + '\t%d' % (1 if flash else 0)


def read_all(cap, opts=None):
    """-> list of state dicts for every frame of a capture folder (flash filled in)."""
    import glob
    geom = open(os.path.join(cap, 'geometry.bin'), 'rb').read()
    fs = sorted(glob.glob(os.path.join(cap, 'frames', 'frame_*.bin')))
    states, prev = [], None
    for p in fs:
        s = read_cstate(p, geom, opts)
        s['flash'] = flash_between(prev, s) if prev is not None else False
        states.append(s)
        prev = s
    return states


if __name__ == '__main__':
    import glob
    cap = sys.argv[1]
    geom = open(os.path.join(cap, 'geometry.bin'), 'rb').read()
    fs = sorted(glob.glob(os.path.join(cap, 'frames', 'frame_*.bin')))
    prev = None
    for n, p in enumerate(fs):
        s = read_cstate(p, geom)
        key = (s['has_text'], s['seq76'], s['title'], tuple(s['rows']), s['hot'], s['tab'], s['dots'])
        if key != prev:
            print(n, s['guest'], ('no text' if not s['has_text'] else summary(s)))
            prev = key
