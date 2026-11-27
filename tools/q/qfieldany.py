r"""q field OFF --any [--reads | --writes] [--func F] [--limit N] [--json]: every function with a load / store whose displacement is OFF, whatever the
base register (S111 QFIX5). For offsets the field index cannot place (a global block reached through a getter's return, `lwz r11,15207(r3)` with r3 = G),
so no grep over the listing is needed.

It is the listing scan `q range` does, over the whole generated listing: a text prefilter over each generated .cpp finds the instruction comments
(`\t// lwz r11,15207(r3)`) whose operand ends `OFF(rN)` (decimal or 0xHEX), then qrange.disp_of + qdecomp.LOADS / STORES decide (same rule as `q range --loads /
--stores`: any width, update forms too, indexed forms carry no displacement and never match). The function is the DEFINE_REX_FUNC above the line. No dataflow, no
base type: a hit says "this function reads / writes +OFF through some register", confirm the base in `q decomp FUNC` / `q asm FUNC`.
--reads = loads, --writes = stores, neither = both. The total of the same scan is always printed; a cut at --limit (default 500, none under --where) prints
the total and the exact call for the rest. ~10-20 s for the whole listing. --type / --vtable / --global / --root-name / --width / --strict do not apply here.
"""
import bisect
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import qdecomp  # noqa: E402
import qrange  # noqa: E402

DEF_RX = re.compile(rb'^DEFINE_REX_FUNC\((sub_[0-9A-Fa-f]{8})\)', re.M)
INS_RX = re.compile(rb'^\t// ([A-Za-z][\w.+\-]*) ?([^\n]*)$', re.M)


def prefilter_rx(off):
    """the operand text `OFF(rN)` in decimal or hex (0x0 padding allowed), anchored to the end of the comment line"""
    neg = '-' if off < 0 else ''
    a = abs(off)
    forms = [str(a), '0x0*%x' % a]
    return re.compile(('(?i)[,\\s]%s(?:%s)\\(r\\d+\\)\\s*$' % (neg, '|'.join(forms))).encode(), re.M)


def scan(off, want_loads, want_stores, gen=None, only=None):
    """-> {func addr: {'reads': n, 'writes': n, 'first': 'mn ops'}} over generated/default/*.cpp"""
    gen = gen or qdecomp.GEN
    rx = prefilter_rx(off)
    res = {}
    for fname in sorted(os.listdir(gen)):
        if not fname.endswith('.cpp'):
            continue
        with open(os.path.join(gen, fname), 'rb') as f:
            data = f.read()
        if not rx.search(data):
            continue
        defs = [(m.start(), m.group(1).decode()) for m in DEF_RX.finditer(data)]
        starts = [d[0] for d in defs]
        for m in INS_RX.finditer(data):
            line = m.group(0)
            if not rx.search(line):
                continue
            mn, ops = m.group(1).decode(), m.group(2).decode().strip()
            kind = 'reads' if mn in qdecomp.LOADS else 'writes' if mn in qdecomp.STORES else None
            if kind is None or (kind == 'reads' and not want_loads) or (kind == 'writes' and not want_stores):
                continue
            if qrange.disp_of(ops) != off:
                continue
            k = bisect.bisect_right(starts, m.start()) - 1
            if k < 0:
                continue
            fa = int(defs[k][1][4:], 16)
            if only is not None and fa != only:
                continue
            r = res.setdefault(fa, {'reads': 0, 'writes': 0, 'first': f'{mn} {ops}'})
            r[kind] += 1
    return res


def answer(a, off, out, parse_func):
    """the q field OFF --any answer in the shape print_any() writes: out['rows'] (cut at a.limit), out['functions'] = the total functions, out['count'] = all accesses"""
    want_loads = a.reads or not a.writes
    want_stores = a.writes or not a.reads
    only = parse_func(a.func) if a.func else None
    res = scan(off, want_loads, want_stores, gen=getattr(a, 'gen', None), only=only)
    fa = sorted(res)
    rows = [{'func': f'sub_{x:08X}', 'reads': res[x]['reads'], 'writes': res[x]['writes'], 'first': res[x]['first']} for x in fa]
    out['any'] = True
    out['off'] = off
    out['functions'] = len(rows)
    out['count'] = sum(r['reads'] + r['writes'] for r in rows)
    out['rows'] = rows[:a.limit]
    out['truncated'] = len(rows) > a.limit
    out['total'] = len(rows)
    if out['truncated']:
        out['rest'] = f"q field {getattr(a, 'call', '')} --limit {len(rows)}".replace('  ', ' ')
    out['note'] = 'listing scan: any base register, no dataflow; indexed forms (lwzx) carry no displacement; confirm the base in q decomp / q asm'
    return out


def print_any(out, fname):
    for r in out['rows']:
        print(f"{fname(int(r['func'][4:], 16))}  R{r['reads']} W{r['writes']}  e.g. {r['first']}")
    print(f"-- {out['functions']} functions, {out['count']} accesses with a load / store at +{out['off']} on any base register (listing scan, not the field index)", file=sys.stderr)
    if out['truncated']:
        print(f"-- showing {len(out['rows'])} of {out['total']} functions (cut at --limit): the rest: {out['rest']}; or narrow with --func F, or `--where REGEX` (filters the WHOLE list, no cut)",
              file=sys.stderr)
