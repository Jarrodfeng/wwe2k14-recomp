"""qdecomp_phi: what flowed into a join, and the audit that no printed name is left undefined (S110 lane QDECFIX).

The rule: every name `q decomp` prints is defined in the same output; where a value cannot be kept exact the line says so
and names the listing lines to read: `?(simplified: see listing :A-:B)`.

* `record` keeps, per join block and register (also ctr / lr / a CR field), the value each predecessor held. qdecomp's
  `merge()` calls it where it makes the join value `rN@J`; before this module those inputs were thrown away.
* `slot` is a placeholder line qdecomp prints under each block header; `audit` replaces it by the definition lines
  (`rN@J = one of: @PRED value, ...`) of the join values the output really uses (all others stay silent: a join with 100
  dead registers prints nothing).
* `audit` also walks the finished text: a printed name with no definition, a raw `crN.x`, a block with no known
  predecessor and the XER carry that is not modelled each get the marker with real listing line numbers.
* `ca_addic` / `ca_use`: the carry bit for the one exact pattern (`addic rX,rY,-1` then `subfe rZ,rZ,rZ`).
Pure data and text; no import of qdecomp (the formatter is passed in).
"""
import re

SLOT = '@@PHI '
MAXSHOW = 12
NAME_RE = re.compile(r'\b((?:r\d+|f\d+|v\d+|ctr|lr|cr\d)@[0-9A-F]{8})\b')
RAWCR_RE = re.compile(r'\bcr\d\.(?:eq|lt|gt|so|un)\b')
NOPRED_RE = re.compile(r'\?[rfv]\d+ \(no known predecessor\)')
CARRY_RE = re.compile(r'\?carry \(not modelled\)')
DEF_LINE_RE = re.compile(r'^\s*((?:r\d+|f\d+|v\d+|ctr|lr|cr\d)@[0-9A-F]{8}) = one of:')
HEADER_RE = re.compile(r'^\s*@([0-9A-F]{8})\s+\[listing')
SITE_RE = re.compile(r'^\s*\+([0-9A-F]+)\s|// \+([0-9A-F]+)\b')

NO_PRED = 'no known predecessor'
# mnemonics that write XER.CA (consumers read it first): after them the carry is not known unless set below
CA_WRITERS = frozenset(['addic', 'addic.', 'subfic', 'subfc', 'addc', 'subfe', 'adde', 'addze', 'subfze', 'addme',
                        'sraw', 'srawi', 'srad', 'sradi', 'addco', 'subfco', 'addeo', 'subfeo', 'addzeo', 'subfzeo',
                        'addmeo', 'mtxer', 'mcrxr'])
CA_USERS = frozenset(['subfe', 'adde', 'addze', 'subfze', 'addme'])
CA_UNKNOWN = ('raw', '?carry (not modelled)')


# ------------------------------------------------------------------ what flowed into a join
def reset(fn, join):
    """a merge at `join` is about to run again: its old records are stale"""
    if not hasattr(fn, 'phis'):
        fn.phis = {}
    fn.phis[join] = {}


def record(fn, kind, key, join, inputs):
    """keep the value each predecessor held. key: 'r30', 'f1', 'ctr', 'lr', 'cr6'; inputs: [(pred addr | None for the
    function entry, value or None)]"""
    if not hasattr(fn, 'phis'):
        fn.phis = {}
    fn.phis.setdefault(join, {})[key] = (kind, [(p, v) for p, v in inputs])


def slot(addr, pad):
    return f'{SLOT}{addr:08X}|{pad}'


# ------------------------------------------------------------------ carry
def ca_addic(a, imm):
    """the carry out of `addic rD,rA,imm`: exact only for imm == -1 (it carries when rA != 0), else not known"""
    if imm == -1:
        return ('op', '!=', a, ('c', 0))
    return None


def ca_use(mn, args, ca):
    """the value of a carry-consuming op. `x - 1 + CA` for `subfe x,x,x`; `rA + CA` for addze; else the op with the
    carry as an argument; the carry unknown prints a marker token"""
    if ca is None:
        return ('op', mn, *args, CA_UNKNOWN)
    if mn == 'subfe' and len(args) == 2 and args[0] == args[1]:
        return ('op', '-', ca, ('c', 1))
    if mn == 'addze' and len(args) == 1:
        return ('op', '+', args[0], ca)
    return ('op', mn, *args, ca)


# ------------------------------------------------------------------ listing ranges
def join_range(fn, j):
    """listing lines (a, b) to read for a join at j: its predecessors' first lines to its own last line"""
    blk = fn.by_start.get(j)
    if blk is None:
        return fn.insns[0].line, fn.insns[-1].line
    lo = blk.line
    for p, _kind in fn.preds.get(j, []):
        pb = fn.by_start.get(p)
        if pb is not None:
            lo = min(lo, pb.line)
    return lo, blk.insns[-1].line


def _flat(fn, key, inputs, seen):
    """a chain of joins of the SAME register (an input that is itself `key`'s join value) flattened to its leaves: each
    leaf's value is `key`'s at the end of that block. A join already open, or another register's join (its blocks and
    values are that register's, not `key`'s), stays a name, defined on its own line."""
    out = []
    for p, v in inputs:
        if isinstance(v, tuple) and v and v[0] in ('phi', 'crphi') and v[1] == key and (v[1], v[2]) not in seen:
            inner = getattr(fn, 'phis', {}).get(v[2], {}).get(v[1])
            if inner is not None:
                out += _flat(fn, key, inner[1], seen | {(v[1], v[2])})
                continue
        out.append((p, v))
    return out


def _def_text(fn, key, j, rec, fmt):
    leaves = []
    for pv in _flat(fn, key, rec[1], {(key, j)}):
        if pv not in leaves:
            leaves.append(pv)
    parts = []
    for p, v in leaves[:MAXSHOW]:
        lab = '@entry' if p is None else f'@{p:08X}'
        s = f'{lab} {"?" if v is None else fmt(v)}'
        if p is not None and p >= j:
            s += ' (previous iteration)'
        parts.append(s)
    text = 'one of: ' + ', '.join(parts)
    if len(leaves) > MAXSHOW:
        a, b = join_range(fn, j)
        for p, _v in leaves[MAXSHOW:]:      # a flattened leaf's block is not a direct predecessor of j: widen to it
            pb = fn.by_start.get(p) if p is not None else None
            a = min(a, pb.line if pb is not None else fn.insns[0].line)
            b = max(b, pb.insns[-1].line if pb is not None else b)
        text += f', +{len(leaves) - MAXSHOW} more not shown: see listing :{a}-:{b}'
    return text


def _natural(name):
    reg, addr = name.split('@')
    m = re.match(r'([a-z]+)(\d*)', reg)
    return (m.group(1), int(m.group(2) or 0), addr)


# ------------------------------------------------------------------ the audit
def audit(lines, fn, fmt):
    """-> (new lines, number of names marked). Replaces each slot by the definitions of the join values the output uses,
    then marks every name that is still not defined."""
    phis = getattr(fn, 'phis', {})
    defs = {}
    seen = set()
    work = []

    def want(text):
        for m in NAME_RE.finditer(text):
            n = m.group(1)
            if n not in seen:
                seen.add(n)
                work.append(n)

    for l in lines:
        if not l.startswith(SLOT):
            want(l)
    while work:
        n = work.pop()
        key, j = n.split('@')
        j = int(j, 16)
        rec = phis.get(j, {}).get(key)
        if rec is None:
            continue
        defs[n] = _def_text(fn, key, j, rec, fmt)
        want(defs[n])
    out = []
    placed = set()
    for l in lines:
        if l.startswith(SLOT):
            j, pad = l[len(SLOT):].split('|', 1)
            suffix = '@' + j
            for n in sorted((n for n in defs if n.endswith(suffix)), key=_natural):
                out.append(f'{pad}  {n} = {defs[n]}')
                placed.add(n)
            continue
        out.append(l)
    line_of = {i.addr: i.line for i in fn.insns}
    cur = None
    nmark = 0
    for i, l in enumerate(out):
        m = HEADER_RE.match(l)
        if m:
            cur = fn.by_start.get(int(m.group(1), 16))
        bad = []
        lo, hi = None, None
        for m in NAME_RE.finditer(l):
            n = m.group(1)
            if n not in placed:
                a, b = join_range(fn, int(n.split('@')[1], 16))
                bad.append(n)
                lo, hi = min(a, lo or a), max(b, hi or b)
        for rx in (RAWCR_RE, NOPRED_RE, CARRY_RE):
            for m in rx.finditer(l):
                bad.append(m.group(0))
                if cur is None:
                    a, b = fn.insns[0].line, fn.insns[-1].line
                else:
                    a, b = cur.line, cur.insns[-1].line
                    s = SITE_RE.search(l)
                    if s:
                        site = fn.addr + int(s.group(1) or s.group(2), 16)
                        b = line_of.get(site, b)
                lo, hi = min(a, lo or a), max(b, hi or b)
        if bad:
            out[i] = f'{l}  ?(simplified: see listing :{lo}-:{hi})'
            nmark += len(bad)
    return out, nmark


def undefined(lines, marked_ok=False):
    """names printed in `lines` that no line defines (the selftest's general check). marked_ok: a name on a line that
    carries the `?(simplified` marker does not count"""
    defd = set()
    for l in lines:
        m = DEF_LINE_RE.match(l)
        if m:
            defd.add(m.group(1))
    bad = []
    for l in lines:
        if marked_ok and '?(simplified' in l:
            continue
        for m in NAME_RE.finditer(l):
            if m.group(1) not in defd:
                bad.append(m.group(1))
        for rx in (RAWCR_RE, NOPRED_RE, CARRY_RE):
            bad += [m.group(0) for m in rx.finditer(l)]
    return sorted(set(bad))
