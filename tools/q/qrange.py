"""q range: the functions whose start lies in an address range, optionally only those that load / store a field offset or call a function (S111 QVCALLVT).

    py tools/q/q.py range LO HI [--loads OFF] [--stores OFF] [--calls sub_X|0xADDR|NAME] [--exclude LO-HI] [--limit N] [--json]

LO..HI is [LO, HI): a function is in when its start address is. --loads OFF = a load with displacement OFF (`lwz r11,1056(r3)`, any width, any base
register, update forms too); --stores OFF the same for stores; --calls X = a `bl X` or a tail `b X` (a direct call; an indirect call has no target);
several filters must ALL hold (a function with a load of OFF and a call of X). --exclude LO-HI drops the functions starting in that sub-range (the
scan "users of +1056 outside 0x82B55xxx" = `range 0x82000000 0x83000000 --loads 1056 --exclude 0x82B55000-0x82B56000`).
Reads each function's own listing (qdecomp.read_listing, the reader q decomp uses): no dataflow, no cache, ~10 ms per function; a range of 1500 functions
takes about 15 s. Indexed forms (lwzx) carry no displacement and never match; a displacement written 0x420 is read as 1056. Output: one line per function
(start, size, name, db name) with the matching instructions' addresses. Errors: `q: ...` on stderr, exit 2.
"""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import qdecomp  # noqa: E402


class RangeError(Exception):
    pass


def hx(a):
    return f'0x{a:08X}'


def num(s):
    try:
        return int(s.strip(), 0)
    except ValueError:
        raise RangeError(f'{s!r}: not a number (decimal or 0xHEX)')


def disp_of(ops):
    """the displacement of the memory operand `OFF(rN)` of an instruction's operand text, or None"""
    for tok in qdecomp.split_ops(ops):
        m = qdecomp.MEM_RE.match(tok)
        if m:
            return qdecomp.int_of(m.group(1))
    return None


def hits_of(insns, loads, stores, calls):
    """-> {filter name: [(addr, text)]} for the filters given (an empty list = that filter does not hold)"""
    out = {}
    if loads is not None:
        out['loads'] = [(i.addr, f'{i.mn} {i.ops}') for i in insns if i.mn in qdecomp.LOADS and disp_of(i.ops) == loads]
    if stores is not None:
        out['stores'] = [(i.addr, f'{i.mn} {i.ops}') for i in insns if i.mn in qdecomp.STORES and disp_of(i.ops) == stores]
    if calls is not None:
        out['calls'] = [(i.addr, f'{i.mn} {i.ops}') for i in insns if i.mn in ('bl', 'b') and qdecomp.branch_target(i) == calls]
    return out


def scan(con, lo, hi, loads=None, stores=None, calls=None, exclude=None, gen=None):
    """-> (functions in the range, matches [{addr, name, size, file, hits}], functions without a listing)"""
    gen = gen or qdecomp.GEN
    rows = con.execute('SELECT addr, name, file, line FROM funcs WHERE addr >= ? AND addr < ? ORDER BY addr', (lo, hi)).fetchall()
    try:
        size = {r[0]: r[1] for r in con.execute('SELECT addr, size FROM funcs WHERE addr >= ? AND addr < ?', (lo, hi))}
    except Exception:  # noqa: BLE001 - an index without a size column
        size = {}
    n = 0
    res, nolist = [], 0
    filt = loads is not None or stores is not None or calls is not None
    for r in rows:
        a = r[0]
        if exclude and exclude[0] <= a < exclude[1]:
            continue
        n += 1
        if not r[2]:
            nolist += 1
            continue
        rec = {'addr': a, 'name': r[1], 'size': size.get(a), 'file': r[2], 'hits': {}}
        if filt:
            try:
                insns = qdecomp.read_listing(os.path.join(gen, r[2]), r[3], r[1], a)[0]
            except (qdecomp.DecompError, OSError):
                nolist += 1
                continue
            h = hits_of(insns, loads, stores, calls)
            if not all(h.values()):
                continue
            rec['hits'] = h
        res.append(rec)
    return n, res, nolist


def db_names():
    try:
        import qstruct
        return qstruct.db_names()
    except Exception:  # noqa: BLE001 - the names are decoration
        return {}


def main(argv, con):
    ap = argparse.ArgumentParser(prog='q range', description=__doc__.split('\n')[0])
    ap.add_argument('lo', help='first address (0x82B00000)')
    ap.add_argument('hi', help='end address, exclusive')
    ap.add_argument('--loads', metavar='OFF', help='only functions with a load whose displacement is OFF')
    ap.add_argument('--stores', metavar='OFF', help='only functions with a store whose displacement is OFF')
    ap.add_argument('--calls', metavar='sub_X', help='only functions with a direct call (bl / tail b) to X')
    ap.add_argument('--exclude', metavar='LO-HI', help='drop the functions starting in this sub-range')
    ap.add_argument('--limit', type=int, default=200)
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args(argv)
    lo, hi = num(a.lo), num(a.hi)
    if hi <= lo:
        raise RangeError(f'{a.hi} is not above {a.lo}')
    exclude = None
    if a.exclude:
        p = a.exclude.split('-')
        if len(p) != 2:
            raise RangeError(f'--exclude {a.exclude!r}: LO-HI')
        exclude = (num(p[0]), num(p[1]))
    calls = None
    if a.calls:
        import qstruct
        try:
            calls = qstruct.parse_addr(a.calls, qstruct._resolver(con))
        except qstruct.StructError as e:
            raise RangeError(str(e))
    n, res, nolist = scan(con, lo, hi, num(a.loads) if a.loads else None, num(a.stores) if a.stores else None, calls, exclude)
    names = db_names()
    if a.json:
        print(json.dumps({'lo': hx(lo), 'hi': hx(hi), 'functions_in_range': n, 'matches': len(res), 'without_listing': nolist,
                          'functions': [{'func': f'sub_{r["addr"]:08X}', 'name': r['name'], 'db_name': names.get(r['addr']), 'size': r['size'],
                                         'hits': {k: [{'at': hx(x), 'insn': t} for x, t in v] for k, v in r['hits'].items()}} for r in res[:a.limit]]}, indent=1))
        return 0
    filt = ' '.join(f'--{k} {v}' for k, v in (('loads', a.loads), ('stores', a.stores), ('calls', a.calls), ('exclude', a.exclude)) if v)
    print(f'q range {hx(lo)} {hx(hi)} {filt}: {len(res)} of {n} function(s) start in the range' + (f' ({nolist} without a readable listing)' if nolist else ''))
    for r in res[:a.limit]:
        sz = f'{r["size"]:6d}' if r['size'] else '     ?'
        extra = '  ' + '; '.join(f'{k} ' + ', '.join(f'{x:08X}' for x, _t in v[:4]) + (f' +{len(v) - 4}' if len(v) > 4 else '') for k, v in r['hits'].items()) if r['hits'] else ''
        print(f'  sub_{r["addr"]:08X} {sz}  {(names.get(r["addr"]) or r["name"] or "")[:40]:40s}{extra}')
    if len(res) > a.limit:
        print(f'  ... {len(res) - a.limit} more (--limit N)')
    return 0
