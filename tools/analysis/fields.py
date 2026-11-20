r"""Field index over the recompiled PPC listings: who loads / stores base+displacement (S103 lane T2).

Built from the PPC comments in the local generated code (generated/default, never committed), with list_function.py's
address rules (4 bytes per PPC comment line, `loc_XXXXXXXX:` labels re-sync). The index is a regenerable sqlite file,
out/analysis/fields/fields.sqlite (built on first use, ~1-3 min, all cores; rebuilt when the generated files change).

  py -I tools\analysis\fields.py store 616 [--range 0x82BD0000-0x82C14000]   every store to displacement 616
  py -I tools\analysis\fields.py load 0x268                                  every load (decimal or hex; negative ok)
  py -I tools\analysis\fields.py any 3984 --width 4 --tag a0                 loads + stores, filtered
  py -I tools\analysis\fields.py func sub_82BD4F88                           every memory op of one function
  py -I tools\analysis\fields.py build [--rebuild]   /   stat

Filters: --range LO-HI (instruction address), --func sub_X, --width N (bytes), --tag TEXT (substring of the base tag),
--eff (match tag_offset + displacement instead of the raw displacement: `addi r11,r3,600` then `16(r11)` is eff 616 on a0),
--sp (include r1-based stack ops, off by default), --limit N, --reg rN (base register).

Base tag (cheap, LINEAR and HEURISTIC: one forward pass per function, no control flow; a join can make it wrong, and a
`bl` clears r0 and r3-r12): `a0`..`a7` = the function's argument registers r3..r10 and their copies (`mr`, `addi` adds to
the tag offset); `[a0+480]` = a pointer loaded from 480(a0) (up to 3 levels); `sp` = r1. Use the tag to narrow, the raw
displacement and the listing to confirm.

Indexed forms (lwzx, stwx, ...) are indexed with displacement NULL: query them with `x`. `lea` lists `addi rD,rS,imm`
(rS not r0 / r1): the way a field's ADDRESS is handed on (a sub-object at +3936 passed to a callee). Vector forms
(lvx128, stvx128) are not indexed."""
import argparse
import glob
import multiprocessing
import os
import re
import sqlite3
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
SCHEMA = 5

FUNC_RE = re.compile(r'^DEFINE_REX_FUNC\((sub_[0-9A-Fa-f]{8})\)', re.M)
TOKEN_RE = re.compile(r'^(?:\t// (.*)|(loc_[0-9A-F]{8}):|(DEFINE_REX_FUNC\(sub_[0-9A-Fa-f]{8}\)))', re.M)
MEM_RE = re.compile(r'^(lbz|lhz|lha|lwz|lwa|ld|lfs|lfd|stb|sth|stw|std|stfs|stfd)(u?)\s+([rf]\d+),\s*(-?(?:0x)?[0-9A-Fa-f]+)\((r\d+)\)\s*$')
IDX_RE = re.compile(r'^(lbz|lhz|lha|lwz|lwa|ld|lfs|lfd|stb|sth|stw|std|stfs|stfd)(u?)x\s+([rf]\d+),\s*(r\d+),\s*(r\d+)\s*$')
ADDI_RE = re.compile(r'^addi\s+(r\d+),\s*(r\d+),\s*(-?(?:0x)?[0-9A-Fa-f]+)\s*$')
WIDTH = {'lbz': 1, 'lhz': 2, 'lha': 2, 'lwz': 4, 'lwa': 4, 'ld': 8, 'lfs': 4, 'lfd': 8, 'stb': 1, 'sth': 2, 'stw': 4, 'std': 8, 'stfs': 4, 'stfd': 8}
NOWRITE = ('st', 'cmp', 'b', 'mt', 'dcb', 'icbi', 'sync', 'eieio', 'isync', 'tw', 'td', 'nop', 'lwsync', 'ptesync', 'sc', 'cr')
VOLATILE = [0] + list(range(3, 13))


def generated_dir():
    for cand in (os.environ.get('WWE_GENERATED'), os.path.join(ROOT, 'generated', 'default')):
        if cand and glob.glob(os.path.join(cand, 'wwe2k14_recomp.*.cpp')):
            return cand
    try:   # a worktree: the generated code lives in the main checkout
        common = subprocess.run(['git', '-C', ROOT, 'rev-parse', '--git-common-dir'], capture_output=True, text=True).stdout.strip()
        main = os.path.dirname(os.path.abspath(os.path.join(ROOT, common)))
        cand = os.path.join(main, 'generated', 'default')
        if glob.glob(os.path.join(cand, 'wwe2k14_recomp.*.cpp')):
            return cand
    except OSError:
        pass
    sys.exit('generated/default not found (set WWE_GENERATED)')


def cache_path():
    return os.path.join(ROOT, 'out', 'analysis', 'fields', 'fields.sqlite')


def signature(gen):
    parts = []
    for p in sorted(glob.glob(os.path.join(gen, 'wwe2k14_recomp.*.cpp'))):
        st = os.stat(p)
        parts.append(f'{os.path.basename(p)}:{st.st_size}:{int(st.st_mtime)}')
    return f'v{SCHEMA};' + ';'.join(parts)


def regnum(tok):
    return int(tok[1:]) if tok[0] == 'r' and tok[1:].isdigit() else None


def scan_function(name, items):
    """items: list of ('c', text) / ('l', addr). Returns (rows, n_instr, bad_labels). rows: (func, addr, kind, mn, width,
    freg, base, disp, tag, tagoff, insn)."""
    start = int(name[4:], 16)
    address = start
    tags = {3 + i: (f'a{i}', 0) for i in range(8)}
    tags[1] = ('sp', 0)
    rows = []
    count = bad = 0
    for kind, val in items:
        if kind == 'l':
            if val != address:
                bad += 1
                address = val
            continue
        text = val.strip()
        m = MEM_RE.match(text)
        mi = IDX_RE.match(text) if not m else None
        ma = ADDI_RE.match(text) if not m and not mi else None
        if mi:
            base_mn, upd, reg, ra, rb = mi.groups()
            ls = 'l' if base_mn[0] == 'l' else 'st'
            a_, b_ = int(ra[1:]), int(rb[1:])
            t = tags.get(b_) or (tags.get(a_) if a_ else None)
            mn = base_mn + upd + 'x'
            rows.append((start, address, 'L' if ls == 'l' else 'S', mn, WIDTH[base_mn], reg, b_, None,
                         t[0] if t else None, t[1] if t else None, text))
            if ls == 'l':
                d = regnum(reg)
                if d is not None:
                    tags.pop(d, None)
        elif ma:
            rd, rs, imm = ma.groups()
            imm = int(imm, 16) if '0x' in imm else int(imm)
            d_, s_ = int(rd[1:]), int(rs[1:])
            t = tags.get(s_)
            if s_ not in (0, 1):
                rows.append((start, address, 'A', 'addi', 0, rd, s_, imm, t[0] if t else None, t[1] if t else None, text))
            if s_ in tags and s_ != 0:
                tags[d_] = (tags[s_][0], tags[s_][1] + imm)
            else:
                tags.pop(d_, None)
        elif m:
            base_mn, upd, reg, disp, base = m.groups()
            ls = 'l' if base_mn[0] == 'l' else 'st'
            disp = int(disp, 16) if '0x' in disp else int(disp)
            b = int(base[1:])
            t = tags.get(b)
            mn = base_mn + upd
            rows.append((start, address, 'L' if ls == 'l' else 'S', mn, WIDTH[base_mn], reg, b, disp,
                         t[0] if t else None, t[1] if t else None, text))
            if ls == 'l':
                d = regnum(reg)
                if d is not None:
                    if mn in ('lwz', 'ld') and t and t[0].count('[') < 3:
                        tags[d] = (f'[{t[0]}+{t[1] + disp}]', 0)
                    elif mn in ('lwz', 'ld') and b not in (0, 1):
                        tags[d] = (f'[*+{disp}]', 0)   # a pointer loaded from +disp of a base we cannot name
                    else:
                        tags.pop(d, None)
            if upd and b != 1:
                tags.pop(b, None)
        else:
            parts = text.split(None, 1)
            mn = parts[0].rstrip('.')
            ops = parts[1].split(',') if len(parts) > 1 else []
            if mn == 'bl' or mn == 'bctrl' or mn == 'blrl':
                for r in VOLATILE:
                    tags.pop(r, None)
            elif not mn.startswith(NOWRITE) and ops:   # (addi is handled above)
                d = regnum(ops[0].strip())
                if d is not None:
                    src = regnum(ops[1].strip()) if len(ops) > 1 else None
                    if mn == 'mr' and src is not None and src in tags:
                        tags[d] = tags[src]
                    else:
                        tags.pop(d, None)
        address += 4
        count += 1
    return rows, count, bad


def scan_file(path):
    text = open(path, encoding='utf-8').read()
    rows, funcs, instrs, bad = [], 0, 0, 0
    cur, items = None, []
    for m in TOKEN_RE.finditer(text):
        c, label, fn = m.groups()
        if fn:
            if cur:
                r, n, b = scan_function(cur, items)
                rows += r; instrs += n; bad += b; funcs += 1
            cur = 'sub_' + fn[len('DEFINE_REX_FUNC(sub_'):-1].upper()
            items = []
        elif cur:
            if label:
                items.append(('l', int(label[4:], 16)))
            else:
                items.append(('c', c))
    if cur:
        r, n, b = scan_function(cur, items)
        rows += r; instrs += n; bad += b; funcs += 1
    return rows, funcs, instrs, bad


def build(gen, db_path):
    files = sorted(glob.glob(os.path.join(gen, 'wwe2k14_recomp.*.cpp')))
    os.makedirs(os.path.dirname(db_path), exist_ok=True)
    tmp = '%s.%d.tmp' % (db_path, os.getpid())   # S105 E review 7: one temp per process (two cold builds no longer remove each other's open file)
    if os.path.exists(tmp):
        os.remove(tmp)
    con = sqlite3.connect(tmp)
    con.execute('create table ops(func integer, addr integer, kind text, mn text, width integer, freg text, base integer, '
                'disp integer, tag text, tagoff integer, insn text)')
    con.execute('create table meta(k text primary key, v text)')
    t0 = time.time()
    nf = ni = nb = nr = 0
    with multiprocessing.Pool(min(16, os.cpu_count() or 4)) as pool:
        for rows, funcs, instrs, bad in pool.imap_unordered(scan_file, files):
            con.executemany('insert into ops values (?,?,?,?,?,?,?,?,?,?,?)', rows)
            nf += funcs; ni += instrs; nb += bad; nr += len(rows)
    con.execute('create index i_disp on ops(kind, disp)')
    con.execute('create index i_func on ops(func, addr)')
    con.execute('create index i_addr on ops(addr)')
    for k, v in (('signature', signature(gen)), ('functions', nf), ('instructions', ni), ('label_mismatches', nb), ('ops', nr)):
        con.execute('insert into meta values (?,?)', (k, str(v)))
    con.commit()
    con.close()
    os.replace(tmp, db_path)
    print(f'built {db_path}: {nf} functions, {ni} instructions, {nr} memory ops, {nb} label mismatches, {time.time() - t0:.0f}s', file=sys.stderr)


def open_db(rebuild=False):
    db_path = cache_path()
    gen = generated_dir()
    if not rebuild and os.path.exists(db_path):
        con = sqlite3.connect(db_path)
        try:
            sig = con.execute("select v from meta where k='signature'").fetchone()
            if sig and sig[0] == signature(gen):
                return con
        except sqlite3.Error:
            pass
        con.close()
    # S105 E (s103_T2d.fields_check timed out at 600 s on a cold index): build()'s Pool is a spawn pool; started from another script's top level (fields_check.py runs
    # open_db() at import) each worker re-imports THAT script, which calls open_db() again: the build never ends. Another script builds through this file's own __main__.
    mainf = getattr(sys.modules.get('__main__'), '__file__', None)
    if mainf and os.path.abspath(mainf) != os.path.abspath(__file__):
        r = subprocess.run([sys.executable, '-I', os.path.abspath(__file__), 'build', '--rebuild'])
        if r.returncode:
            raise RuntimeError('fields.py build failed (exit %d)' % r.returncode)
    else:
        build(gen, db_path)
    return sqlite3.connect(db_path)


def parse_range(s):
    lo, hi = s.split('-')
    return int(lo, 0), int(hi, 0)


def run_query(con, kind, disp, a):
    where, args = [], []
    if kind in ('L', 'S', 'A'):
        where.append('kind=?'); args.append(kind)
    elif a.cmd == 'any':
        where.append("kind<>'A'")
    if disp == 'x':
        where.append('disp is null')
    elif disp is not None:
        if a.eff:
            where.append('tagoff is not null and tagoff+disp=?')
        else:
            where.append('disp=?')
        args.append(disp)
    if a.range:
        lo, hi = parse_range(a.range)
        where.append('addr>=? and addr<?'); args += [lo, hi]
    if a.func:
        where.append('func=?'); args.append(int(a.func.replace('sub_', ''), 16))
    if a.width:
        where.append('width=?'); args.append(a.width)
    if a.tag:
        where.append('tag like ?'); args.append(f'%{a.tag}%')
    if a.reg:
        where.append('base=?'); args.append(int(a.reg.lstrip('r')))
    if not a.sp and not a.func and not a.reg:
        where.append('base<>1')
    q = ('select func, addr, kind, insn, tag, tagoff from ops'
         + (' where ' + ' and '.join(where) if where else '') + ' order by addr limit ?')
    args.append(a.limit)
    return con.execute(q, args).fetchall()


def show(rows):
    cur = None
    for func, addr, kind, insn, tag, tagoff in rows:
        if func != cur:
            cur = func
            print(f'sub_{func:08X}')
        t = f'  [base {tag}{tagoff:+d}]' if tag else ''
        print(f'  0x{addr:08X}  {kind} {insn}{t}')
    funcs = len({r[0] for r in rows})
    print(f'-- {len(rows)} ops in {funcs} functions', file=sys.stderr)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('cmd', choices=['load', 'store', 'any', 'lea', 'func', 'build', 'stat'])
    ap.add_argument('arg', nargs='?')
    ap.add_argument('--range'); ap.add_argument('--func'); ap.add_argument('--width', type=int)
    ap.add_argument('--tag'); ap.add_argument('--reg'); ap.add_argument('--eff', action='store_true')
    ap.add_argument('--sp', action='store_true'); ap.add_argument('--limit', type=int, default=2000)
    ap.add_argument('--rebuild', action='store_true')
    a = ap.parse_args()
    con = open_db(a.rebuild or a.cmd == 'build' and a.rebuild)
    if a.cmd == 'build':
        return
    if a.cmd == 'stat':
        print(dict(con.execute('select k, v from meta where k<>"signature"').fetchall()))
        return
    if a.cmd == 'func':
        a.func = a.func or a.arg
        if not a.func:
            sys.exit('func needs sub_XXXXXXXX')
        show(run_query(con, None, None, a))
        return
    if a.arg is None:
        sys.exit('give a displacement')
    kind = {'load': 'L', 'store': 'S', 'any': None, 'lea': 'A'}[a.cmd]
    show(run_query(con, kind, 'x' if a.arg == 'x' else int(a.arg, 0), a))


if __name__ == '__main__':
    multiprocessing.freeze_support()
    main()
