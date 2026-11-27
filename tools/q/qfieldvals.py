r"""q field OFF --writes --values: per store, the VALUE stored and the OTHER fields the same function stores through the same root (S111 QWRITERS).

    py tools/q/q.py field OFF --writes --values [--co N] [--json]

One line per store: the function, the store address, the width, `val` and `co:`.
  val   the immediate stored, traced back through li / lis / ori / addi / mr (the register folding of build_fields.step: a constant
        survives a join only when every path agrees); else `reg rN` (the register's name; `reg rN=a1+0` when it holds a rooted value).
  co:   the other offsets that function stores through the SAME root (the field index's W rows with the same func and root id), as
        +OFF=value, the CO nearest stores (by address) first-capped, then `(+K more)`. Whole-function, not "within the block".
The stores come from the field index (no decomp). Only the value needs the listing: the writer functions' text is cut out of
generated/default in ONE regex pass over the files (the field index keeps no value, and a schema bump would rename fields.s2.db,
which other readers open by name) and run through build_fields.analyze with a wrapped step that records the source register's state
before each store. A function whose text is not found says `val ?`. Every value is a lead to confirm in the listing (rule 4).
"""
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import build_fields as bf   # noqa: E402

CO_DEFAULT = 6
_ORIG = bf.step
_CAP = {}


def _wrapped(st, ins, out, func):
    """record, on the emitting pass, the state of a store's source register before the store runs"""
    if out is not None and ins[1].startswith('st'):
        m = bf.MEM_RE.match(ins[1] + ' ' + ins[2])
        if m and m.group(1).startswith('s'):
            reg = m.group(3)
            r = bf.regn(reg)
            _CAP[ins[0]] = (reg, st.get(r) if r is not None else None, m.group(5))
    return _ORIG(st, ins, out, func)


def trace_stores(func, text, trace=True):
    """{store addr: (reg, imm or None, root text or None, base register)} for one function's text (DEFINE_REX_FUNC ... closing brace).
    trace=False: the register only (the planted twin of the selftest: no value tracer)."""
    _CAP.clear()
    items = []
    for m in bf.TOKEN_RE.finditer(text):
        c, label, fn, helper, goto = m.groups()
        if fn:
            items = []
        elif label:
            items.append(('l', int(label, 16)))
        elif c is not None:
            items.append(('c', c))
        elif helper:
            items.append(('h',))
        elif goto:
            items.append(('g', int(goto, 16)))
    bf.step = _wrapped
    try:
        bf.analyze(func, items)
    finally:
        bf.step = _ORIG
    out = {}
    for addr, (reg, v, base) in _CAP.items():
        if trace and v is not None and v[0] == 'K':
            out[addr] = (reg, v[1], None, base)
        elif trace and v is not None and v[0] != 's':
            out[addr] = (reg, None, '%s%+d' % (v[0], v[1]), base)
        else:
            out[addr] = (reg, None, None, base)
    _CAP.clear()
    return out


def find_texts(gen, funcs):
    """{func: text} for the wanted functions: one regex pass over every generated source (bytes, no decode of what is not wanted)"""
    want = sorted(funcs)
    got = {}
    if not want:
        return got
    rx = re.compile(rb'DEFINE_REX_FUNC\(sub_(' + b'|'.join(b'%08X' % f for f in want) + rb')\)')
    for path in sorted(glob.glob(os.path.join(gen, 'wwe2k14_recomp.*.cpp'))):
        with open(path, 'rb') as f:
            data = f.read()
        for m in rx.finditer(data):
            end = data.find(b'\n}', m.end())
            got[int(m.group(1), 16)] = data[m.start(): end if end >= 0 else len(data)].decode('utf-8', 'replace')
        if len(got) == len(want):
            break
    return got


def fmt_imm(v):
    """a stored constant: small signed decimal, else hex"""
    if v >= 0x80000000:
        s = v - (1 << 32)
        if s >= -0x8000:
            return str(s)
    return str(v) if v <= 0xFFFF else '0x%X' % v


def val_text(t):
    """the value of one traced store -> text; t = (reg, imm, root) or None"""
    if t is None:
        return '?'
    reg, imm, root = t[:3]
    if imm is not None:
        return fmt_imm(imm)
    return 'reg %s' % reg + ('=%s' % root if root else '')


def val_json(t):
    if t is None:
        return {'imm': None, 'reg': None, 'root': None, 'text': '?'}
    return {'imm': t[1], 'reg': t[0], 'root': t[2], 'text': val_text(t)}


def print_rows(out, fname):
    """one line per store: function, address, width, offset, root, val, co. Reads (rw R / A) keep the plain line."""
    for r in out['rows']:
        head = '%s  %s  %s w%d%s +%d  root %s' % (fname(int(r['func'][4:], 16)), r['addr'], r['rw'], r['width'], 'f' if r['float'] else '', r['off'], r['root'])
        if r['rw'] != 'W':
            print(head)
            continue
        co = ', '.join('%s+%d=%s' % ('' if c['root'] == r['root'] else c['root'], c['off'], c['val']['text']) for c in r['co'])
        if r['co_more']:
            co += (', ' if co else '') + '(+%d more)' % r['co_more']
        print('%s  val %s  co: %s' % (head, r['val']['text'], co or '-'))
    print('-- %d accesses in %d functions' % (out['count'], out['functions']) + (' (truncated: --limit)' if out['truncated'] else ''), file=sys.stderr)


def attach(con, rows, gen, co_n=CO_DEFAULT, trace=True, any_root=False):
    """rows: the dicts of qfield.cmd_off, each with 'func' (sub_X) 'addr' (0x..) 'rw' 'off' and 'rid' (the root id). Adds 'val' and 'co' to every W row.
    co = the function's other W rows through the same root (any_root: through any root, tagged with its root). Root 'u' is every base the fold could not
    name: there co keeps only the stores through the same base REGISTER (a lead: the register may have been re-pointed between)."""
    wr = [r for r in rows if r['rw'] == 'W']
    funcs = {int(r['func'][4:], 16) for r in wr}
    texts = find_texts(gen, funcs)
    traced = {f: trace_stores(f, t, trace) for f, t in texts.items()}
    for r in wr:
        f = int(r['func'][4:], 16)
        addr = int(r['addr'], 16)
        tr = traced.get(f)
        r['val'] = val_json(tr.get(addr) if tr is not None else None)
        co = []
        base = tr[addr][3] if tr is not None and addr in tr else None
        for a, off, w, rt in con.execute("select r.addr, r.off, r.width, ro.text from rows r join roots ro on ro.id=r.root "
                                         "where r.func=:f and r.rw='W' and not (r.root=:rid and r.off=:off) and (:any or r.root=:rid) order by r.addr",
                                         {'f': f, 'rid': r['rid'], 'off': r['off'], 'any': int(any_root)}):
            if rt == 'u' and r['root'] == 'u' and base is not None and tr is not None and a in tr and tr[a][3] != base:
                continue
            co.append((abs(a - addr), a, off, w, rt))
        co.sort()
        keep = sorted(co[:co_n], key=lambda x: x[1])
        r['co'] = [{'root': rt, 'off': off, 'addr': '0x%08X' % a, 'width': w, 'val': val_json(tr.get(a) if tr is not None else None)}
                   for _d, a, off, w, rt in keep]
        r['co_more'] = len(co) - len(keep)
    return rows
