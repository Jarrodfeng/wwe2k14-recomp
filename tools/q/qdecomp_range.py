"""q decomp FUNC --range A..B (S111 WRITERLINT): print only one region of a decomp listing, with the function's header line.

  A..B   each end is a function offset (`+0x45C`, `+45C`, `45C`: hex, as the listing prints its `+1C` column) or a game address (`0x8283A5E8`, `8283A5E8`:
         eight hex digits, minus the function's start). `A..` runs to the end, `..B` from the start.

A body line belongs to the region by its offset: an instruction line `  +1C  ...`, a structure line with a `// +10` comment, or a block head `@8283A604`; a line with no
offset of its own (a closing brace, a facts comment) takes the offset of the line above it. Pure text, no index: the unit is tools\\q\\decomp_selftest.py (range case).
"""
import re

INSN_RE = re.compile(r'^\s*\+([0-9A-Fa-f]+)\s')
COMMENT_RE = re.compile(r'//\s*\+([0-9A-Fa-f]+)\b')
BLOCK_RE = re.compile(r'^\s*@([0-9A-Fa-f]{8})\b')
END_RE = re.compile(r'^\+?(?:0x)?([0-9A-Fa-f]+)$')


class RangeError(ValueError):
    pass


def _end(tok, base):
    tok = tok.strip()
    if not tok:
        return None
    m = END_RE.match(tok)
    if not m:
        raise RangeError('range end %r is not +0x45C / 45C / 0x8283A5E8' % tok)
    n = int(m.group(1), 16)
    is_addr = (not tok.startswith('+')) and len(m.group(1)) >= 8
    if is_addr:
        if n < base:
            raise RangeError('address 0x%08X is before the function start 0x%08X' % (n, base))
        return n - base
    return n


def parse(spec, base):
    """'+0x45C..+0x4C0' -> (lo, hi) offsets; an open end is 0 / a huge number."""
    if '..' not in spec:
        raise RangeError('--range wants A..B (offsets like +0x45C..+0x4C0, or addresses), got %r' % spec)
    a, b = spec.split('..', 1)
    lo, hi = _end(a, base), _end(b, base)
    lo = 0 if lo is None else lo
    hi = (1 << 40) if hi is None else hi
    if hi < lo:
        raise RangeError('range %r runs backwards' % spec)
    return lo, hi


def filter_body(lines, lo, hi, base):
    """The body lines whose offset (own, else the line above's) lies in lo..hi."""
    out, cur = [], None
    for ln in lines:
        m = INSN_RE.match(ln) or COMMENT_RE.search(ln)
        if m:
            cur = int(m.group(1), 16)
        else:
            b = BLOCK_RE.match(ln)
            if b:
                cur = int(b.group(1), 16) - base
        if cur is not None and lo <= cur <= hi:
            out.append(ln)
    return out
