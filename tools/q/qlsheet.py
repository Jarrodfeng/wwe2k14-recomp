#!/usr/bin/env python3
r"""Listing sheets (S110): a unit's fact sheet PLUS the decompiled view (`q decomp`) of its biggest functions, for the second
reading pass of units whose first reading said "listing needed: yes".

  py -I tools\q\qlsheet.py LIST_FILE OUT_DIR [--funcs N] [--kb-func K] [--kb-unit K] [--pick connected|size] [--skip-lib]

LIST_FILE: one existing sheet file path per line (the sheets `q read pick` wrote; named UNIT.txt). Writes OUT_DIR\UNIT.txt and
OUT_DIR\batch.lst (the new paths), and prints one line per unit plus the total bytes.

Each output file is: [a `library: Havok (range: between A and B)` line when the unit is third-party library code (qlib.py)], the sheet,
then the listings. (S111 FOLD: the part with the neighbouring units' earlier readings is gone: it lent a reading its confidence.)

Which functions (--pick, default connected): `connected` = by callers (inside the unit + outside + observed), unnamed first, functions under
40 bytes ignored, ties by size (qlsheet_ctx.pick_connected); `size` = by size, unnamed ones first (a function line of the sheet with
nothing after `Ni`). At most --funcs (default 2); each listing is cut at --kb-func (default 4) KB with a line saying how much was cut, and
a unit's listings stop at --kb-unit (default 8) KB.

--skip-lib leaves library units out (nothing written for them) and prints how many, and which libraries.

The output holds DECOMPILED GAME CODE: it is scratch under out\ only, never committed, never shipped (CLAUDE.md rule 1).
"""
import collections
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
FUNC = re.compile(r'^([0-9A-F]{8}) (\d+)B (\d+)i ?(.*)$')
UNIT = re.compile(r'^# unit (\d+) ')


def pick(sheet_text, n):
    """tonight's rule (--pick size): the unit's functions by size, unnamed first"""
    rows = []
    for line in sheet_text.splitlines():
        m = FUNC.match(line)
        if m:
            rows.append((0 if FUNC_unnamed(m.group(4)) else 1, -int(m.group(2)), m.group(1)))
    return [r[2] for r in sorted(rows)[:n]]


def unit_of(sheet_text, path):
    m = UNIT.match(sheet_text.lstrip().splitlines()[0]) if sheet_text.strip() else None
    if m:
        return int(m.group(1))
    b = re.match(r'(\d+)', os.path.basename(path))
    return int(b.group(1)) if b else None


def decomp(addr):
    r = subprocess.run([sys.executable, os.path.join(HERE, 'q.py'), 'decomp', 'sub_' + addr], capture_output=True, text=True,
                       encoding='utf-8', errors='replace', timeout=300)
    return r.stdout if r.returncode == 0 and r.stdout.strip() else '(q decomp failed: %s)\n' % (r.stderr.strip().splitlines() or ['no output'])[-1]


# --skip-lib skips a unit only when TWO independent signals agree: qlib's label AND the first-pass reader's own system for the
# unit. One library class name among many functions labels a whole unit (S110 LSHEET2 hand-back: unit 100, 150 functions, one
# Havok hit, read as creation / menus), so the label alone must never make the second pass skip game code.
LIB_SYSTEMS = {'Havok': ('engine', 'other', 'unknown', 'pose', 'model', ''), 'Wwise': ('audio', 'engine', 'other', 'unknown', ''),
               'Lua': ('engine', 'other', 'unknown', ''), 'Bink': ('screens', 'engine', 'other', 'unknown', ''),
               'zlib': ('engine', 'other', 'unknown', ''), 'LZX': ('engine', 'other', 'unknown', ''), 'CRT': ('engine', 'other', 'unknown', '')}


def reader_system(uid):
    """the newest reading's system of a unit ('' = none stated), or None when the unit has no reading"""
    import sqlite3
    import qread
    if not os.path.exists(qread.READ_DB):
        return None
    c = sqlite3.connect('file:%s?mode=ro' % qread.READ_DB.replace('\\', '/'), uri=True)
    try:
        r = c.execute('select system from unit_read where unit_id=? order by id desc limit 1', (uid,)).fetchone()
    finally:
        c.close()
    return (r[0] or '') if r else None


def FUNC_unnamed(rest):
    """a sheet function line's names field with the tags that are not names removed: [p:kind] (qpattern) and reader names"""
    return not re.sub(r'\[p:[^\]]*\]|read \S+ \[r:[^\]]*\]', '', rest).strip()


def lib_line(r):
    return 'library: %s (%s: %s)' % (r[0], r[1], re.sub(r'\s+', ' ', r[2] or '')[:200])


def main(argv):
    args, skip = [], set()
    for i, a in enumerate(argv):
        if a in ('--funcs', '--kb-func', '--kb-unit', '--pick'):
            skip.add(i + 1)
        if not a.startswith('--') and i not in skip:
            args.append(a)
    if len(args) != 2:
        print(__doc__)
        return 2

    def opt(name, default):
        return int(argv[argv.index(name) + 1]) if name in argv else default
    nfun, kbf, kbu = opt('--funcs', 2), opt('--kb-func', 4), opt('--kb-unit', 8)
    mode = argv[argv.index('--pick') + 1] if '--pick' in argv else 'connected'
    if mode not in ('connected', 'size'):
        print('--pick takes connected or size')
        return 2
    skip_lib = '--skip-lib' in argv
    out_dir = args[1]
    if os.path.normcase(os.path.abspath(out_dir)).find(os.sep + 'out' + os.sep) < 0:
        print('refused: OUT_DIR must be under out\\ (decompiled game code is scratch, rule 1)')
        return 1
    import qlib
    import qlsheet_ctx
    os.makedirs(out_dir, exist_ok=True)
    data, lib = qlsheet_ctx.Data(), qlib.open_lib()
    paths, total, libs, nlib = [], 0, collections.Counter(), 0
    for src in [ln.strip() for ln in open(args[0], encoding='utf-8') if ln.strip()]:
        text = open(src, encoding='utf-8').read()
        uid = unit_of(text, src)
        lr = qlib.lookup(uid, lib) if uid is not None else None
        if lr:
            nlib += 1
            libs[lr[0]] += 1
            if skip_lib:
                rs = reader_system(uid)
                if rs is not None and rs in LIB_SYSTEMS.get(lr[0], ()):
                    print('%s  library %s (%s), reader system %s: skipped' % (os.path.basename(src), lr[0], lr[1], rs or '(none)'))
                    continue
                print('%s  library %s (%s) but the reader says %s: KEPT (the two signals disagree)' % (os.path.basename(src), lr[0], lr[1], rs))
                nlib -= 1
                libs[lr[0]] -= 1
        parts = [(lib_line(lr) + '\n' if lr else '') + text.rstrip('\n')]
        parts += ['', '# LISTINGS (the tool-made decompiled view of this unit\'s biggest functions; `[x]` = a memory load; a line the tool'
                  ' could not keep exact says so)']
        used, shown = 0, []
        if mode == 'size' or uid is None:
            addrs = pick(text, nfun)
        else:
            addrs = ['%08X' % a for a in qlsheet_ctx.pick_connected(uid, nfun, data)]
        for addr in addrs:
            if used >= kbu * 1024:
                break
            body = decomp(addr)
            cut = ''
            if len(body) > kbf * 1024:
                cut = '... CUT: %d of %d bytes of this listing shown\n' % (kbf * 1024, len(body))
                body = body[:kbf * 1024].rsplit('\n', 1)[0] + '\n'
            parts.append('## listing %s\n%s%s' % (addr, body, cut))
            used += len(body)
            shown.append(addr)
        dst = os.path.join(out_dir, os.path.basename(src))
        data_out = '\n'.join(parts) + '\n'
        with open(dst, 'w', encoding='utf-8', newline='\n') as f:
            f.write(data_out)
        paths.append(os.path.abspath(dst))
        total += len(data_out.encode('utf-8'))
        print('%s  sheet %d B  + listings %s  = %d B%s' % (
            os.path.basename(src), len(text.encode('utf-8')), ' '.join(shown) or 'none',
            len(data_out.encode('utf-8')), '  [library %s]' % lr[0] if lr else ''))
    with open(os.path.join(out_dir, 'batch.lst'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(paths) + '\n')
    print('%d units, %d bytes total -> %s' % (len(paths), total, out_dir))
    if nlib:
        print('%d library unit(s) in the list%s: %s' % (nlib, ', skipped' if skip_lib else ' (kept; --skip-lib leaves them out)',
                                                      ' '.join('%s %d' % kv for kv in sorted(libs.items()))))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
