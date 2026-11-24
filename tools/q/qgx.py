r"""q game extras (S105 lane QG): five small answers the game-side index lacked.

    py tools/q/q.py switch FUNC [--limit N]        the full case -> target table of every jump-table switch in FUNC, with the
                                                   `li` constants / stores of constants / calls inside each case body
                                                   (`q decomp FUNC --cases` = the same)
    py tools/q/q.py text ID [--lang N] [--all]     text id -> string, from the game's own string tables (pac\string.pac,
                                                   docs/formats/string_pac.md)
    py tools/q/q.py csdt [ERA [KEY]] [--lang L]    the evtdata.pac /CSDT caption tables (docs/formats/evtdata_csdt.md): the
                                                   directory, one era's captions (key, start / end frame, text) or one key
    py tools/q/q.py field OFF --stores ...         qstores.py (indexed stores + memcpy writers added to `q field OFF --writes`)
    py tools/q/q.py ours includes HEADER --included-by | --transitive     qgx.cmd_includes_graph

Game files are read from the player's own dump (read-only); nothing is written to the repo.
"""
import json
import os
import re
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
REPO = os.path.normpath(os.path.join(HERE, '..', '..'))
MAIN = r'C:\Users\Jarrod\wwe2k14-recomp'


class GxError(Exception):
    pass


# ----------------------------------------------------------------------------------------------------------- switch
INS = re.compile(r'^\t// ([A-Za-z][\w.+\-]*) ?(.*?)\s*$')
LABEL = re.compile(r'^loc_([0-9A-F]{8}):')
CASE = re.compile(r'^\s*case (\d+):')
GOTO = re.compile(r'goto loc_([0-9A-F]{8});')
M32 = 0xFFFFFFFF


def _int(s):
    try:
        return int(s.strip(), 0)
    except ValueError:
        return None


def _name(con, addr, cache):
    if addr not in cache:
        r = con.execute('SELECT name FROM funcs WHERE addr = ?', (addr,)).fetchone()
        cache[addr] = r['name'] if r else 'sub_%08X' % addr
    return cache[addr]


def parse_switches(lines):
    """lines = the function body (listing). -> [{'site': addr or None, 'range': hi or None, 'default': addr or None,
    'cases': [(n, target)], 'labels': {addr: line_index}}]. A switch = the `switch (` ... `}` block the recompiler writes under a
    `// bctr` comment; the bound check before it (cmplwi + bgt / bge) gives the default."""
    labels = {}
    for i, l in enumerate(lines):
        m = LABEL.match(l)
        if m:
            labels[int(m.group(1), 16)] = i
    out = []
    i = 0
    while i < len(lines):
        if lines[i].startswith('\tswitch ('):
            msr = re.match(r'^	switch \(ctx\.r(\d+)\.', lines[i])
            sreg = int(msr.group(1)) if msr else None
            sw = {'cases': [], 'default': None, 'range': None, 'line': i, 'reg': sreg, 'bias': 0, 'src': None}
            j = i + 1
            cur = None
            while j < len(lines) and lines[j] != '\t}':
                m = CASE.match(lines[j])
                if m:
                    cur = int(m.group(1))
                elif lines[j].strip().startswith('default:'):
                    cur = 'default'
                else:
                    g = GOTO.search(lines[j])
                    if g and cur is not None:
                        if cur == 'default':
                            sw['default'] = int(g.group(1), 16)
                        else:
                            sw['cases'].append((cur, int(g.group(1), 16)))
                        cur = None
                j += 1
            # the bound check: scan back for cmplwi / cmpwi + the branch after it
            # (H2/L8) only a compare of the SWITCH register counts; a biased switch (`addi rX,rY,-N` before the bound) has case
            # values = index + N
            k = i - 1
            while k >= max(0, i - 40):
                m = INS.match(lines[k])
                if m and m.group(1) in ('cmplwi', 'cmpwi'):
                    ops = [x.strip() for x in m.group(2).split(',')]
                    if sreg is not None and ops[-2] != 'r%d' % sreg:
                        k -= 1
                        continue
                    sw['range'] = _int(ops[-1])
                    for q in range(k + 1, min(k + 4, i)):
                        b = INS.match(lines[q])
                        if b and b.group(1).rstrip('+-') in ('bgt', 'bge', 'bnl'):
                            sw['default'] = sw['default'] or _int(b.group(2).split(',')[-1])
                    for q in range(k - 1, max(k - 6, -1), -1):       # the instruction that wrote the compared register
                        a = INS.match(lines[q])
                        if not a:
                            continue
                        ao = [x.strip() for x in a.group(2).split(',')]
                        if a.group(1) in ('addi', 'subi', 'addic', 'addic.') and len(ao) == 3 and ao[0] == ops[-2]:
                            v = _int(ao[2])
                            if v is not None:
                                sw['bias'] = -v if a.group(1) != 'subi' else v
                                sw['src'] = ao[1]
                            break
                        if ao and ao[0] == ops[-2] and not a.group(1).startswith(('st', 'cmp', 'b')):
                            break
                    break
                k -= 1
            out.append(sw)
            i = j
        i += 1
    return out, labels


def case_body(lines, labels, start_line, other_targets, con, cache, cap=60, seed=None):
    """Straight-line scan of one case body: the constants loaded, constants stored, calls, and how it ends."""
    regs = dict(seed or {})
    consts, stores, calls = [], [], []
    end = ''
    n = 0
    cond = 0
    j = start_line
    while j < len(lines) and n < cap:
        l = lines[j]
        m = LABEL.match(l)
        if m and j != start_line and int(m.group(1), 16) in other_targets:
            end = 'falls into @%s' % m.group(1)
            break
        m = INS.match(l)
        j += 1
        if not m:
            continue
        n += 1
        mn, ops = m.group(1).rstrip('.'), m.group(2)
        o = [x.strip() for x in ops.split(',')]
        if mn in ('b', 'blr', 'bctr'):
            end = 'tail @' + o[0][2:].upper() if mn == 'b' and o[0].startswith('0x') else mn
            break
        d = int(o[0][1:]) if o and re.match(r'^r\d+$', o[0]) else None
        if mn == 'li' and len(o) == 2 and _int(o[1]) is not None:
            regs[d] = _int(o[1]) & M32
            consts.append((d, regs[d]))
        elif mn == 'lis' and len(o) == 2 and _int(o[1]) is not None:
            regs[d] = (_int(o[1]) << 16) & M32
        elif mn in ('ori', 'oris', 'addi', 'addis') and len(o) == 3 and re.match(r'^r\d+$', o[1]):
            s = int(o[1][1:])
            v = _int(o[2])
            if v is not None and (s in regs or (mn in ('addi', 'addis') and s == 0)):
                base = regs.get(s, 0)
                if mn == 'ori':
                    regs[d] = (base | v) & M32
                elif mn == 'oris':
                    regs[d] = (base | (v << 16)) & M32
                elif mn == 'addi':
                    regs[d] = (base + v) & M32
                else:
                    regs[d] = (base + (v << 16)) & M32
                if s != d or True:
                    consts.append((d, regs[d]))
            elif d is not None:
                regs.pop(d, None)
        elif mn == 'mr' and len(o) == 2 and re.match(r'^r\d+$', o[1]):
            s = int(o[1][1:])
            if s in regs:
                regs[d] = regs[s]
            else:
                regs.pop(d, None)
        elif mn in ('stw', 'stb', 'sth', 'std') and len(o) == 2 and d in regs:
            stores.append((mn, regs[d], o[1]))
        elif mn == 'bctrl':
            calls.append(('(indirect: vtable / pointer)', ''))
            for r in (0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12):
                regs.pop(r, None)
        elif mn.startswith('b') and mn not in ('bl', 'blrl'):
            cond += 1
        elif mn == 'bl':
            t = _int(o[0])
            if t is not None:
                args = ['r%d=%s' % (r, _fmtv(regs[r])) for r in (3, 4, 5, 6, 7, 8) if r in regs]
                calls.append((_name(con, t, cache), ' '.join(args)))
            for r in (0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12):
                regs.pop(r, None)
        elif d is not None and not mn.startswith(('st', 'cmp', 'b', 'mt', 'dcb')):
            regs.pop(d, None)
    if not end:
        end = 'cap' if n >= cap else 'eof'
    return {'consts': consts, 'stores': stores, 'calls': calls, 'end': end, 'cond': cond}


def _fmtv(v):
    if v >= 0xFFFF0000:
        return str(v - (1 << 32))
    return str(v) if v < 1024 else '0x%X' % v


def fn_constants(body, upto):
    """(M5) registers whose EVERY write in the function is the same `li` constant, the li lying before line `upto`:
    {reg: value}. A bl / bctrl counts as a write of r0 / r3-r12. A lead, not a proof (a register never written on some path is a parameter)."""
    vals, first = {}, {}
    for j, l in enumerate(body):
        m = INS.match(l)
        if not m:
            continue
        mn = m.group(1).rstrip('.')
        o = [x.strip() for x in m.group(2).split(',')]
        if mn in ('bl', 'bctrl'):
            for r in (0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12):
                vals.setdefault(r, set()).add(None)
            continue
        if not o or not re.match(r'^r\d+$', o[0]) or mn.startswith(('st', 'cmp', 'b', 'mt', 'dcb')):
            continue
        d = int(o[0][1:])
        if mn == 'li' and len(o) == 2 and _int(o[1]) is not None:
            vals.setdefault(d, set()).add(_int(o[1]) & M32)
            first.setdefault(d, j)
        else:
            vals.setdefault(d, set()).add(None)
    return {r: next(iter(v)) for r, v in vals.items() if len(v) == 1 and None not in v and first.get(r, upto) < upto}


def cmd_switch(con, addr, limit=40):
    """-> (header, [lines]) for every switch in the function at `addr`."""
    f = con.execute('SELECT * FROM funcs WHERE addr = ?', (addr,)).fetchone()
    if f is None or not f['file']:
        raise GxError('0x%08X has no generated body' % addr)
    path = os.path.join(_gen(), f['file'])
    if not os.path.exists(path):
        raise GxError('generated source %s is missing (generated/ is local only)' % path)
    with open(path, encoding='utf-8', errors='replace') as fh:
        allv = fh.read().split('\n')
    i = (f['line'] or 0) - 1
    if not (0 <= i < len(allv) and allv[i].startswith('DEFINE_REX_FUNC(%s)' % f['name'])):
        i = next((k for k, l in enumerate(allv) if l.startswith('DEFINE_REX_FUNC(%s)' % f['name'])), -1)
        if i < 0:
            raise GxError('%s not found in %s' % (f['name'], f['file']))
    j = i + 1
    while j < len(allv) and allv[j] != '}':
        j += 1
    body = allv[i + 1:j]
    sws, labels = parse_switches(body)
    cache = {}
    res = {'func': f['name'], 'addr': addr, 'switches': []}
    for sw in sws:
        fnc = fn_constants(body, sw['line'])
        targets = {t for _, t in sw['cases']}
        groups = {}
        for n, t in sw['cases']:
            groups.setdefault(t, []).append(n)
        rows = []
        for t, ns in sorted(groups.items(), key=lambda kv: kv[1][0]):
            b = case_body(body, labels, labels[t] + 1, targets - {t}, con, cache, seed=fnc) if t in labels else \
                {'consts': [], 'stores': [], 'calls': [], 'end': 'target not in function'}
            rows.append({'cases': ns, 'target': t, **b})
        res['switches'].append({'range': sw['range'], 'default': sw['default'], 'ncases': len(sw['cases']), 'groups': rows,
                                'bias': sw['bias'], 'src': sw['src'], 'reg': sw['reg'], 'fn_consts': {'r%d' % r: v for r, v in sorted(fnc.items())}})
    return res


def print_switch(r, limit=40):
    if not r['switches']:
        print('%s: no jump-table switch (no `switch (` block under a bctr in the listing)' % r['func'])
        return
    for k, sw in enumerate(r['switches']):
        bias = sw.get('bias') or 0
        print('%s switch #%d: %d cases, bound %s (index), default %s%s' % (r['func'], k, sw['ncases'],
              sw['range'] if sw['range'] is not None else '?', ('@%08X' % sw['default']) if sw['default'] else '-',
              ('; switch on %s %+d: case VALUE = index %+d' % (sw['src'], -bias, bias)) if bias else ''))
        for g in sw['groups'][:limit]:
            ns = [n + bias for n in g['cases']]
            lab = ','.join(map(str, ns)) if len(ns) <= 8 else '%d..%d (%d)' % (ns[0], ns[-1], len(ns))
            if bias:
                lab += ' [idx %d..%d]' % (g['cases'][0], g['cases'][-1]) if len(g['cases']) > 1 else ' [idx %d]' % g['cases'][0]
            seen, cs = set(), []
            for reg, v in g['consts']:
                if (reg, v) not in seen:
                    seen.add((reg, v))
                    cs.append('r%d=%s' % (reg, _fmtv(v)))
            parts = []
            if cs:
                parts.append('li ' + ' '.join(cs[:8]) + (' ...' if len(cs) > 8 else ''))
            if g['stores']:
                parts.append('st ' + ' '.join('%s %s->%s' % (m, _fmtv(v), a) for m, v, a in g['stores'][:4]))
            if g['calls']:
                parts.append('calls ' + '; '.join(('%s(%s)' % c) if c[1] else c[0] for c in g['calls'][:3]))
            print('  %-14s -> @%08X  %s  [%s%s]' % (lab, g['target'], ' | '.join(parts) or '(no constants)', g['end'],
                                                  (', %d cond branch%s' % (g['cond'], '' if g['cond'] == 1 else 'es')) if g.get('cond') else ''))
        if len(sw['groups']) > limit:
            print('  ... %d more targets (--limit)' % (len(sw['groups']) - limit))
        notes = ['each body is a LINEAR scan: a call behind a conditional branch looks unconditional and merged paths show both values (leads: confirm in `q asm`)']
        if sw.get('fn_consts'):
            notes.append('registers seeded from function-wide single li constants: ' + ' '.join('%s=%s' % (a, _fmtv(b)) for a, b in sw['fn_consts'].items()))
        print('  (' + '; '.join(notes) + ')')


_GEN = []


def _gen():
    if not _GEN:
        import build_fields as bf
        _GEN.append(bf.gen_dir(REPO))
    return _GEN[0]


def switch_main(argv, con, resolve):
    import argparse
    ap = argparse.ArgumentParser(prog='q switch', description=__doc__.split('\n\n')[0])
    ap.add_argument('func')
    ap.add_argument('--limit', type=int, default=40)
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--cases', action='store_true')   # `decomp FUNC --cases` passes it through
    ap.add_argument('--callers', type=int, default=0)   # decomp flag, ignored here
    a = ap.parse_args(argv)
    addr, note = resolve(con, a.func)
    if note:
        print('(%s)' % note)
    r = cmd_switch(con, addr, a.limit)
    if a.json:
        print(json.dumps(r, indent=1))
    else:
        print_switch(r, a.limit)
    return 0


# ----------------------------------------------------------------------------------------------------------- pac readers
def _wm30_pac():
    d = os.path.join(REPO, 'tools', 'wm30')
    if d not in sys.path:
        sys.path.insert(0, d)
    import pac
    return pac


def _game_root():
    for r in (REPO, MAIN):
        p = os.path.join(r, 'game_data', 'extracted')
        if os.path.isdir(p):
            return p
    raise GxError('game_data\\extracted not found (the player\'s own dump)')


def epac_dir(data):
    """[(name4, start_sector, size_256)] of an EPAC pac's directory section at 0x800 (count LE32 at +4, 12-byte entries from +12);
    empty slots dropped. docs/formats/evtdata_csdt.md, string_pac.md."""
    if data[:4] != b'EPAC':
        raise GxError('not an EPAC pac')
    n = struct.unpack_from('<I', data, 0x804)[0]
    out = []
    for k in range(min(n, 4096)):
        o = 0x80C + 12 * k
        if o + 12 > len(data):
            break
        nm = data[o:o + 4]
        st, sz = struct.unpack_from('<II', data, o + 4)
        if sz:
            out.append((nm.decode('latin-1'), st, sz))
    return out


def epac_entry(data, st, sz):
    return data[0x4000 + st * 0x800: 0x4000 + st * 0x800 + sz * 256]


def parse_text_table(blob):
    """The string table (docs/formats/string_pac.md): u32 0, u32 COUNT, COUNT x (offset, length, id), strings. -> {id: str}."""
    if len(blob) < 8:
        return {}
    cnt = struct.unpack_from('<I', blob, 4)[0]
    if struct.unpack_from('<I', blob, 0)[0] != 0 or 8 + 12 * cnt > len(blob):
        return {}
    out = {}
    for i in range(cnt):
        off, ln, tid = struct.unpack_from('<III', blob, 8 + 12 * i)
        if off + ln <= len(blob):
            out.setdefault(tid, blob[off:off + ln].split(b'\0')[0].decode('utf-8', 'replace'))
    return out


def string_pacs():
    """[(label, path)] base string.pac first, then the newest DLC string pack (cumulative: the base texts plus every pack up to it)."""
    import glob
    root = _game_root()
    out = [('string.pac', os.path.join(root, 'pac', 'string.pac'))]
    base = os.path.dirname(root)
    dl = sorted(glob.glob(os.path.join(base, 'dlc', '**', 'string_dlc*.pac'), recursive=True), key=lambda p: os.path.basename(p).lower())
    if dl:
        out.append((os.path.basename(dl[-1]), dl[-1]))
    return out


def lang_entries(dirv, lang):
    """SDB entry names for one language: the last two characters = the language index (0000 / 0100 .. 1400 / 5000 are language 0)
    [L: from the names: 5000-5008 are the nine name tables and share their ids; 0 = the English set, the content reads English]."""
    return [nm for nm, _, _ in dirv if nm[2:] == '%02d' % lang]


def cmd_text(tid, lang=0, allf=False):
    pac = _wm30_pac()
    hits = []
    for label, path in string_pacs():
        if not os.path.exists(path):
            continue
        data = pac.load(path)
        dv = epac_dir(data)
        for nm, st, sz in dv:
            if not allf and nm[2:] != '%02d' % lang:
                continue
            for p, leaf in pac.walk(epac_entry(data, st, sz)):
                t = parse_text_table(leaf)
                if tid in t:
                    hits.append({'pac': label, 'entry': nm, 'leaf': p, 'lang': int(nm[2:]) if nm[2:].isdigit() else None, 'text': t[tid]})
        if hits and not allf:
            break
    return hits


def text_main(argv):
    import argparse
    ap = argparse.ArgumentParser(prog='q text', description='text id -> string from the game string tables')
    ap.add_argument('id')
    ap.add_argument('--lang', type=int, default=0)
    ap.add_argument('--all', action='store_true', help='every language / pack, not just the first hit in --lang')
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args(argv)
    tid = _int(a.id)
    if tid is None:
        raise GxError('text id must be a number')
    h = cmd_text(tid, a.lang, a.all)
    if a.json:
        print(json.dumps(h, indent=1))
        return 0
    if not h:
        print('q text: id %d not in the string tables (lang %d%s)' % (tid, a.lang, ', all' if a.all else ''))
        return 1
    shown = h if a.all else h[:1]
    for x in shown[:12]:
        print('%d = %r   (%s entry %s leaf %s, lang %s)' % (tid, x['text'], x['pac'], x['entry'], x['leaf'], x['lang']))
    if len(shown) > 12:
        print('... %d more' % (len(shown) - 12))
    elif not a.all and len(h) > 1:
        print('(+%d more tables of lang %d hold this id: --all)' % (len(h) - 1, a.lang))
    return 0


# ----------------------------------------------------------------------------------------------------------- csdt
def evtdata_pac():
    return os.path.join(_game_root(), 'pac', 'evt', 'evtdata.pac')


def csdt_records(blob):
    """The caption table (S105 QG, listing of sub_82A9E650 / sub_82A9EF28 / sub_823456F0): records chained by their own length: key u32 at +0,
    start u16 at +8 and end u16 at +10 (the game doubles both: sub_82A9E5E8 / sub_82A9E5F8), byte length of the rest u16 at +12 (the next record
    is at +16 + that), a flag u16 at +14, NUL-terminated text at +16; the first record (key 0, length 4) is a header; the chain ends at key -1.
    -> [{key, start, end, flag, text, off}]."""
    out = []
    pos = 0
    while pos + 16 <= len(blob):
        key = struct.unpack_from('<I', blob, pos)[0]
        if key == 0xFFFFFFFF:
            break
        st, en, ln, fl = struct.unpack_from('<4H', blob, pos + 8)
        if key != 0:
            txt = blob[pos + 16: pos + 16 + max(ln, 0)].split(b'\0')[0].decode('utf-8', 'replace')
            out.append({'key': key, 'start': (st * 2) & 0xFFFE, 'end': (en * 2) & 0xFFFE, 'flag': fl, 'text': txt, 'off': pos})
        pos += 16 + ln
    return out


def load_csdt(era, lang='U'):
    pac = _wm30_pac()
    data = pac.load(evtdata_pac())
    name = '%s%03d' % (lang, era)
    for nm, st, sz in epac_dir(data):
        if nm == name:
            leaves = list(pac.walk(epac_entry(data, st, sz)))
            return csdt_records(leaves[0][1]), name
    raise GxError('no /CSDT/%s in evtdata.pac' % name)


def csdt_main(argv):
    import argparse
    ap = argparse.ArgumentParser(prog='q csdt', description='evtdata.pac /CSDT caption schedule tables')
    ap.add_argument('era', nargs='?')
    ap.add_argument('key', nargs='?')
    ap.add_argument('--lang', default='U', help='language letter (U I F G S J K)')
    ap.add_argument('--limit', type=int, default=15)
    ap.add_argument('--find', type=int, help='which era (0-5) holds this key')
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args(argv)
    pac = _wm30_pac()
    if a.find is not None:
        for e in range(6):
            recs, nm = load_csdt(e, a.lang)
            for r in recs:
                if r['key'] == a.find:
                    print('key %d: /CSDT/%s  start %d end %d  %r' % (a.find, nm, r['start'], r['end'], r['text'][:80]))
        return 0
    if a.era is None:
        data = pac.load(evtdata_pac())
        print('evtdata.pac /CSDT: ' + ' '.join('%s(%d)' % (nm, sz) for nm, _, sz in epac_dir(data) if not nm.startswith(('J', 'K', 'F', 'G', 'S', 'I')) or nm == 'ASTL') + '  [other languages: --lang I F G S J K]')
        return 0
    recs, nm = load_csdt(int(a.era), a.lang)
    if a.key is not None:
        k = _int(a.key)
        recs = [r for r in recs if r['key'] == k]
        if not recs:
            print('q csdt: key %s not in /CSDT/%s' % (a.key, nm))
            return 1
    if a.json:
        print(json.dumps(recs, indent=1))
        return 0
    print('/CSDT/%s: %d records, keys %d..%d' % (nm, len(recs), min(r['key'] for r in recs), max(r['key'] for r in recs)) if recs else '/CSDT/%s: empty' % nm)
    for r in recs[:a.limit]:
        print('  key %-5d frames %5d-%-5d flag %d  %r' % (r['key'], r['start'], r['end'], r['flag'], r['text'][:70]))
    if len(recs) > a.limit and a.key is None:
        print('  ... %d more (--limit)' % (len(recs) - a.limit))
    return 0


# ----------------------------------------------------------------------------------------------------------- include graph
def cmd_includes_graph(root, spec, mode, depth=6):
    """mode 'included-by' = the direct includers (qtext); 'transitive' = every file that reaches HEADER through #include (BFS, with the
    depth), and the same downwards with 'closure' (every file HEADER pulls in)."""
    import qtext
    rel = qtext.find_cpp(root, spec)
    if not rel or isinstance(rel, list):
        raise GxError('%s: ' % spec + ('ambiguous: ' + ', '.join(rel[:6]) if rel else 'no such C++ file under src / tools / probes'))
    names = qtext.by_name(root)
    edges_in, edges_out = {}, {}
    for p in qtext.walk(root, qtext.CPP):
        r2 = qtext.rel_of(root, p)
        for ln in qtext.read_lines(p):
            m = qtext.INC.match(ln)
            if m:
                res = qtext.resolve_inc(root, r2, m.group(1), m.group(2), names)
                if res:
                    t = res.split(' ')[0]
                    edges_out.setdefault(r2, set()).add(t)
                    edges_in.setdefault(t, set()).add(r2)
    adj = edges_in if mode in ('included-by', 'transitive') else edges_out
    seen = {rel: 0}
    frontier = [rel]
    d = 0
    while frontier and d < (1 if mode == 'included-by' else depth):
        d += 1
        nxt = []
        for f in frontier:
            for g in sorted(adj.get(f, ())):
                if g not in seen:
                    seen[g] = d
                    nxt.append(g)
        frontier = nxt
    rows = sorted(((v, k) for k, v in seen.items() if k != rel))
    return {'file': rel, 'mode': mode, 'rows': rows}


def print_includes_graph(r, limit=60):
    kind = {'included-by': 'included directly by', 'transitive': 'reached (transitively) from', 'closure': 'pulls in'}[r['mode']]
    srcs = [k for v, k in r['rows'] if k.endswith(('.cpp', '.cc'))]
    print('%s: %s %d files (%d are .cpp translation units)' % (r['file'], kind, len(r['rows']), len(srcs)))
    for v, k in r['rows'][:limit]:
        print('  d%d %s' % (v, k))
    if len(r['rows']) > limit:
        print('  ... %d more (--limit)' % (len(r['rows']) - limit))


def includes_graph_main(argv, root):
    import argparse
    ap = argparse.ArgumentParser(prog='q ours includes')
    ap.add_argument('cmd')
    ap.add_argument('file')
    ap.add_argument('--included-by', action='store_true')
    ap.add_argument('--transitive', action='store_true')
    ap.add_argument('--closure', action='store_true', help='every file FILE pulls in, transitively')
    ap.add_argument('--limit', type=int, default=60)
    ap.add_argument('--root', default=root)
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--tools', action='store_true')
    a = ap.parse_args(argv)
    mode = 'closure' if a.closure else 'transitive' if a.transitive else 'included-by'
    r = cmd_includes_graph(a.root, a.file, mode)
    if a.json:
        print(json.dumps(r, indent=1))
    else:
        print_includes_graph(r, a.limit)
    return 0


# ----------------------------------------------------------------------------------------------------------- q.py hook
def wants(argv):
    """True when q.py should hand this command line to qgx (the only code in q.py's main is `if qgx.wants(argv): return qgx.dispatch(...)`)."""
    if not argv:
        return False
    c = argv[0]
    if c in ('switch', 'text', 'csdt'):
        return True
    if c == 'decomp' and '--cases' in argv:
        return True
    if c == 'field' and '--stores' in argv:
        return True
    return c == 'ours' and len(argv) > 1 and argv[1] == 'includes' and any(x in argv for x in ('--included-by', '--transitive', '--closure'))


def qfield_args(args):
    """the argv qfield's parser gets for `field OFF --stores ...`: qstores-only flags (--var-span N, --all-copies) and --stores itself are stripped (S108 P4a B5:
    qfield exited on --var-span before qstores ran); the default --writes is added"""
    rest, skip = [], False
    for x in args:
        if skip:
            skip = False
        elif x == '--var-span':
            skip = True
        elif x in ('--stores', '--all-copies') or x.startswith('--var-span='):
            pass
        else:
            rest.append(x)
    if '--writes' not in rest and '--reads' not in rest:
        rest.append('--writes')
    return rest


def dispatch(argv, connect, db, resolve):
    try:
        c = argv[0]
        if c == 'switch':
            return switch_main(argv[1:], connect(db), resolve)
        if c == 'decomp':
            return switch_main([x for x in argv[1:] if x != '--fields'], connect(db), resolve)
        if c == 'text':
            return text_main(argv[1:])
        if c == 'csdt':
            return csdt_main(argv[1:])
        if c == 'field':
            import qstores
            import qfield
            qfield.main(qfield_args(argv[1:]))
            return qstores.main(argv[1:], connect(db))
        return includes_graph_main(argv[1:], REPO)
    except GxError as e:
        print('q: %s' % e, file=sys.stderr)
        return 2
    except BrokenPipeError:
        return 0
