"""q args F [--reg rN] [--value V] [--in CALLER] [--all] [--json]   (S111 QARGS)
One line per call site of F (calls and tail calls), showing only the argument registers F itself reads. Each value is folded to ONE of: a constant; an image
address (with the string or db / function name found there); `in_rN`, the caller's own argument passed through; or the expression (`computed` in a histogram).
--reg rN   a histogram of that register over all sites:  r5: 7 x90, 1 x40, computed x20
--value V  only the sites passing V (a number, or text found in the folded value)
--in CALLER  only that caller's sites      --all  no cap on the lines      --json  one document
Values come from qdecomp's interpreter (every argument register at the call, not just those written since the last call); the registers F reads are the
`in_rN` names its decomp mentions (a register F only forwards untouched to a deeper call is not seen). `q vcall OFF --reg rN` is the same for an indirect slot.
Run:  py tools/q/q.py args sub_82BB16A0 --reg r5"""
import collections
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import qdecomp  # noqa: E402

CAP = 60
NUM_RE = re.compile(r'-?\d+|0x[0-9A-Fa-f]+')
IN_RE = re.compile(r'\bin_r(\d+)\b')


class ArgsError(Exception):
    pass


def callee_reads(r):
    """the argument registers (3..10) F reads: the `in_rN` names its decomp (body lines + field lists) mentions -> sorted [3, 4, ...]"""
    text = '\n'.join(r['lines'])
    s = r['summary']
    text += '\n' + '\n'.join(str(x['addr']) for x in s['reads'] + s['writes'])
    return sorted({int(n) for n in IN_RE.findall(text) if 3 <= int(n) <= 10})


def _img_string(image, a):
    d = image.data
    if d is None or a < qdecomp.IMAGE_BASE or a - qdecomp.IMAGE_BASE >= len(d):
        return None
    o = a - qdecomp.IMAGE_BASE
    chunk = d[o:o + 49]
    n = chunk.find(b'\0')
    if n < 3 or any(c < 32 or c > 126 for c in chunk[:n]):
        return None
    return chunk[:n].decode('ascii')


class Folder:
    """folds a decomp value text -> (kind, key, display). kind: const | addr | pass | expr"""

    def __init__(self, con, image=None, db_names=None):
        self.con, self.image, self.db_names = con, image, db_names

    def _db(self):
        if self.db_names is None:
            import q as qmod
            self.db_names = {}
            for a, _f, e in qmod.load_db_entries():
                if e.get('name'):
                    self.db_names.setdefault(a, e['name'])
        return self.db_names

    def addr_note(self, a):
        s = _img_string(self.image, a) if self.image is not None else None
        if s is not None:
            return f'"{s}"'
        r = self.con.execute('SELECT name FROM funcs WHERE addr = ?', (a,)).fetchone()
        nm = self._db().get(a)
        if r and nm:
            return f'{r["name"]} ({nm})'
        if r:
            return r['name']
        return nm or ''

    def fold(self, t, reg):
        t = t.strip()
        if NUM_RE.fullmatch(t):
            v = int(t, 0)
            if 0x82000000 <= v < 0xA0000000:
                note = self.addr_note(v)
                return 'addr', f'0x{v:08X}', f'0x{v:08X}' + (f' {note}' if note else '')
            key = str(v) if -4096 < v < 4096 else f'0x{v & 0xFFFFFFFF:X}'
            return 'const', key, key
        m = re.fullmatch(r'in_r(\d+)', t)
        if m:
            return 'pass', t, t + (" (the caller's own, unchanged)" if f'r{m.group(1)}' == reg else " (the caller's " + t[3:] + ')')
        return 'expr', 'computed', t


def value_matches(kind, key, disp, want):
    w = want.strip()
    if NUM_RE.fullmatch(w):      # a number is compared as a number, against constants and addresses only (never as text inside an expression)
        try:
            return kind in ('const', 'addr') and int(w, 0) == int(key, 0)
        except ValueError:
            return False
    return w.lower() in disp.lower()


def parse_reg(s):
    m = re.fullmatch(r'r?(\d+)', s.strip())
    if not m or not 3 <= int(m.group(1)) <= 10:
        raise ArgsError(f'{s}: not an argument register (r3 .. r10)')
    return f'r{int(m.group(1))}'


def hist_line(reg, vals):
    """vals = [(kind, key, disp)] -> `r5: 7 x90, 1 x40, computed x20`"""
    c = collections.Counter()
    shown = {}
    for kind, key, disp in vals:
        k = key if kind in ('const', 'pass', 'expr') else key
        c[k] += 1
        shown.setdefault(k, disp if kind == 'addr' else key)
    parts = [f'{shown[k]} x{n}' for k, n in sorted(c.items(), key=lambda kv: (-kv[1], kv[0]))]
    return f'{reg}: ' + ', '.join(parts)


def _decomp(con, src, cache):
    if src not in cache:
        try:
            cache[src] = qdecomp.decompile(con, src)
        except qdecomp.DecompError as e:
            cache[src] = str(e)
    return cache[src]


def _who(con, src, site):
    import q as qmod
    return f'{qmod.fname(con, src)}+{site - src:X}'


def collect(con, f, in_caller=None):
    """-> (reads or None, [ {src, site, who, tail, regs|None, why} ]) for every call / tail edge into f"""
    try:
        rf = qdecomp.decompile(con, f)
        reads = callee_reads(rf)
    except qdecomp.DecompError:
        reads = None
    rows = con.execute("SELECT DISTINCT src, site, kind FROM edges WHERE dst = ? AND kind IN ('call', 'tail') ORDER BY src, site", (f,)).fetchall()
    cache, out = {}, []
    for src, site, kind in rows:
        if in_caller is not None and src != in_caller:
            continue
        if src == f:
            continue
        d = _decomp(con, src, cache)
        row = {'src': src, 'site': site, 'who': _who(con, src, site), 'tail': kind == 'tail', 'regs': None, 'why': ''}
        if isinstance(d, str):
            row['why'] = f'no body ({d})'
        else:
            hit = next((x for x in d['summary']['sites'].get(f'{f:08X}', []) if x['site'] == site), None)
            if hit is None or hit.get('regs') is None:
                row['why'] = 'the decomp shows no call at this site'
            else:
                row['regs'] = hit['regs']
        out.append(row)
    return reads, out


def _site_vals(row, regs, folder):
    return {r: folder.fold(row['regs'][r], r) for r in regs if row['regs'] and r in row['regs']}


def args_main(argv, con, resolve):
    import argparse
    ap = argparse.ArgumentParser(prog='q args', description=__doc__.split('\n')[0])
    ap.add_argument('func')
    ap.add_argument('--reg', default=None, metavar='rN', help='histogram of this register over all sites')
    ap.add_argument('--value', default=None, metavar='V', help='only the sites passing V (a number, or text in the folded value)')
    ap.add_argument('--in', dest='in_caller', default=None, metavar='CALLER', help='only that caller')
    ap.add_argument('--all', action='store_true', help='no cap on the lines')
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args(argv)
    f, note = resolve(con, a.func)
    if note:
        print(f'({note})')
    caller = resolve(con, a.in_caller)[0] if a.in_caller else None
    reg = parse_reg(a.reg) if a.reg else None
    import qx6
    folder = Folder(con, qdecomp.Image() if os.path.exists(qdecomp.IMAGE) else None)
    reads, rows = collect(con, f, caller)
    import q as qmod
    name = qmod.fname(con, f)
    regs = [f'r{n}' for n in reads] if reads is not None else [f'r{n}' for n in range(3, 11)]
    if reg:
        regs = [reg]
    out = []
    for row in rows:
        vals = _site_vals(row, regs, folder)
        if a.value is not None and not any(value_matches(k, key, d, a.value) for k, key, d in vals.values()):
            continue
        out.append((row, vals))
    if a.json:
        print(json.dumps({'func': name, 'reads': None if reads is None else [f'r{n}' for n in reads], 'sites_total': len(rows), 'sites': [
            {'caller': r['who'], 'site': f'0x{r["site"]:08X}', 'tail': r['tail'], 'why': r['why'], 'values': {k: {'kind': v[0], 'value': v[2]} for k, v in vs.items()}}
            for r, vs in out]}, indent=1))
        return 0
    rd = 'F reads ' + (', '.join(f'r{n}' for n in reads) if reads else 'no argument register (r3..r10)') if reads is not None else 'F has no body: all of r3..r10 shown'
    print(f'q args {name}: {len(rows)} call sites' + (f' in {a.in_caller}' if caller else '') + f'; {rd}' + (f'; {len(out)} pass {a.value}' if a.value is not None else ''))
    if reg:
        print('  ' + hist_line(reg, [v[reg] for r, v in out if reg in v]) if any(reg in v for r, v in out) else f'  {reg}: no site shows a value')
        unk = [r for r, v in out if reg not in v]
        if unk:
            print(f'  ({len(unk)} site(s) with no value: ' + '; '.join(f'{r["who"]} {r["why"] or "register unset"}' for r in unk[:3]) + (' ...' if len(unk) > 3 else '') + ')')
        if a.value is None:
            print(f'  (the sites: q args {name} --reg {reg} --value V; every site: q args {name})')
        else:
            for r, v in out[:None if a.all else CAP]:
                print(f'  {r["who"]:<32} 0x{r["site"]:08X}{" tail" if r["tail"] else ""}  {reg}={v[reg][2]}')
            if not a.all and len(out) > CAP:
                print(f'  ... {len(out) - CAP} more: q args {a.func} --reg {reg} --value {a.value} --all')
        return 0
    for r, v in out[:None if a.all else CAP]:
        body = ', '.join(f'{k}={x[2]}' for k, x in v.items()) or (r['why'] or 'no argument register read')
        print(f'  {r["who"]:<32} 0x{r["site"]:08X}{" tail" if r["tail"] else "    "}  {body}')
    if not a.all and len(out) > CAP:
        print(f'  ... {len(out) - CAP} more: q args {a.func} --all' + (f' --reg {reg}' if reg else '') + ' (or narrow: --in CALLER, --reg rN, --value V)')
    return 0


def vcall_regs(con, res, reg, value, as_json, limit, off):
    """q vcall OFF --reg rN [--value V]: the register at every listed indirect call site (res = the vcall rows), folded like q args"""
    reg = parse_reg(reg) if reg else None
    if reg is None:
        raise ArgsError('--value needs --reg rN (which register of the call)')
    folder = Folder(con, qdecomp.Image() if os.path.exists(qdecomp.IMAGE) else None)
    cache, rows = {}, []
    for x in res:
        site, fa = int(x['site'], 16), int(x['func'][4:], 16)
        d = _decomp(con, fa, cache)
        v, why = None, ''
        if isinstance(d, str):
            why = f'no body ({d})'
        else:
            hit = next((s for s in d['summary'].get('vsites', []) if s['site'] == site), None)
            if hit is None or not hit.get('regs'):
                why = 'the decomp shows no indirect call at this site'
            elif reg in hit['regs']:
                v = folder.fold(hit['regs'][reg], reg)
            else:
                why = f'{reg} not set'
        rows.append((x, v, why))
    shown = [(x, v, w) for x, v, w in rows if value is None or (v and value_matches(v[0], v[1], v[2], value))]
    if as_json:
        print(json.dumps({'offset': off, 'reg': reg, 'sites': [{'site': x['site'], 'func': x['func'], 'kind': v[0] if v else None, 'value': v[2] if v else None, 'why': w}
                                                              for x, v, w in shown]}, indent=1))
        return 0
    print(f'q vcall 0x{off:X} --reg {reg}: {len(res)} site(s)' + (f'; {len(shown)} pass {value}' if value is not None else ''))
    known = [v for x, v, w in rows if v]
    print('  ' + (hist_line(reg, known) if known else f'{reg}: no site shows a value'))
    if value is not None:
        for x, v, w in shown[:limit]:
            print(f"  {x['site']}  {x['func']:13s} {reg}={v[2]}")
        if len(shown) > limit:
            print(f'  ... {len(shown) - limit} more (--limit N)')
    unk = [(x, w) for x, v, w in rows if not v]
    if unk:
        print(f'  ({len(unk)} site(s) with no value, e.g. {unk[0][0]["site"]}: {unk[0][1]})')
    return 0
