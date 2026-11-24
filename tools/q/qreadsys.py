r"""q read system / systems / store-system / pick --reread (S110 lane RENDERMD2): the system layer over read.s2.db (tools/q/qread.py).

    py tools/q/q.py read pick --reread [--n N] --out DIR     sheets (+ batch.txt) of units answered before `system:` existed
    py tools/q/q.py read system NAME [--out FILE]            the bundle a system-writer reads: every stored reading of that system
    py tools/q/q.py read systems                             per system: units, explanation stored?, STALE (readings newer than it)?
    py tools/q/q.py read store-system ANSWER_FILE [--model NAME]   parse a system-writer's answer into system_read + fact_read

A system-writer answer (the format of .claude/agents/system-writer.md), one or more blocks:
    ## system: pose
    explain:
    <2 to 5 short paragraphs>
    facts:
    - <one-line curiosity> | from: unit N | high|medium|low
    open:
    - <what would have to be read or observed next>
A block that does not parse is reported and SKIPPED. The open lines are kept at the end of the stored explanation.
Everything stored here is a model's reading of tool-made fact sheets: a lead, never a fact.
"""
import collections
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qread  # noqa: E402

FEED_FAMS = ('model', 'material', 'particle', 'light', 'arena', 'crowd', 'camera', 'ui')   # the systems that have a row in the feed bar
OPEN_HEAD = 'Next to read or observe:'


# ---------------------------------------------------------------- state
def latest_units(c):
    """{unit_id: dict} the newest stored answer of each unit"""
    out = {}
    for u, p, cf, bc, ms, ln, md, w, sy, fu in c.execute(
            'select unit_id,purpose,confidence,because,missing,listing_needed,model,when_,system,fun from unit_read '
            'where id in (select max(id) from unit_read group by unit_id) order by unit_id'):
        out[u] = {'purpose': p or '', 'confidence': cf, 'because': bc or '', 'missing': ms or '', 'listing': bool(ln),
                  'model': md or '', 'when': w or '', 'system': sy or '', 'fun': fu or ''}
    return out


def system_state(c):
    """{system: {'n': units, 'newest': date of the newest reading, 'written': (explain, model, when) or None, 'stale': bool}}
    for every system with a reading or an explanation; '' (no system yet) is not a system"""
    lat = latest_units(c)
    out = {}
    for a in lat.values():
        if a['system']:
            e = out.setdefault(a['system'], {'n': 0, 'newest': '', 'written': None, 'stale': False})
            e['n'] += 1
            e['newest'] = max(e['newest'], a['when'])
    for sy, ex, md, w in c.execute('select system,explain,model,when_ from system_read where id in (select max(id) from system_read group by system)'):
        e = out.setdefault(sy, {'n': 0, 'newest': '', 'written': None, 'stale': False})
        e['written'] = (ex, md or '?', w or '')
        e['stale'] = bool(e['newest'] and e['newest'] > (w or ''))
    return out


# ---------------------------------------------------------------- pick --reread
def pick_reread(n, outdir, ctx=None, path=None):
    """-> [(unit_id, bytes)]: units whose newest answer has no system, lowest id first; writes sheets + batch.txt when outdir"""
    c = qread.open_ro(path)
    if c is None:
        return []
    todo = [u for u, a in latest_units(c).items() if not a['system']][:n]
    c.close()
    res = []
    if outdir:
        import qsheet
        ctx = ctx or qsheet.Ctx()
        ctx.hooks()
        os.makedirs(outdir, exist_ok=True)
    for u in todo:
        nbytes = 0
        if outdir:
            text, fl = qsheet.build(ctx, u)
            nbytes = fl['bytes']
            with open(os.path.join(outdir, '%d.txt' % u), 'w', encoding='utf-8') as fh:
                fh.write(text)
        res.append((u, nbytes))
    if outdir:
        with open(os.path.join(outdir, 'batch.txt'), 'w', encoding='utf-8') as fh:
            for u, b in res:
                fh.write('%d.txt\tunit %d\t%d B\treread\n' % (u, u, b))
    return res


# ---------------------------------------------------------------- the bundle
def bundle(system, c, units, bar_row=None, writer_counts=None):
    """the text a system-writer reads. units: a units.s1.db connection (ranges); bar_row: [sites, reached, static, observed, either,
    copier_only, unknown] or None; writer_counts: {unit: {family: read sites its functions were seen writing}} or None"""
    lat = {u: a for u, a in latest_units(c).items() if a['system'] == system}
    o = ['SYSTEM: %s' % system,
         '%d cluster readings stored; newest %s. Every reading below is a model reader\'s reading of a tool-made fact sheet: a LEAD, not a fact.'
         % (len(lat), max([a['when'] for a in lat.values()] or ['-'])),
         '']
    if bar_row:
        o.append('FEED BAR for this family: %d read sites in our renderer, %d reached in recorded runs; of those, writer known by static tables %d, '
                 'observed %d, either %d, copier only %d, no known writer %d.' % tuple(bar_row))
        o.append('')
    for u in sorted(lat):
        a = lat[u]
        rng = ''
        r = units.execute('select lo,hi,n_funcs from unit where unit_id=?', (u,)).fetchone() if units is not None else None
        if r:
            rng = ', 0x%08X..0x%08X, %d funcs' % r
        o.append('## unit %d%s' % (u, rng))
        o.append('confidence: %s   read %s by %s   listing needed: %s' % (a['confidence'], a['when'], a['model'] or '?', 'yes' if a['listing'] else 'no'))
        wc = (writer_counts or {}).get(u)
        if wc:
            o.append('observed writers: functions of this cluster were seen writing read sites of: %s' %
                     ', '.join('%s %d' % (f, k) for f, k in sorted(wc.items(), key=lambda kv: (-kv[1], kv[0]))))
        elif writer_counts is not None:
            o.append('observed writers: none seen in the recorded runs')
        o.append('purpose: %s' % a['purpose'])
        o.append('because: %s' % a['because'])
        o.append('fun: %s' % (a['fun'] or 'none'))
        o.append('missing: %s' % a['missing'])
        for f, nm, ev, cf in c.execute('select func,name,evidence,confidence from name_read where unit_id=? order by id', (u,)):
            o.append('proposed name (a candidate, not the game\'s): sub_%08X -> %s [%s] evidence: %s' % (f, nm, cf, ev))
        o.append('')
    if not lat:
        o.append('(no stored reading names this system yet)')
    return '\n'.join(o) + '\n'


def real_feed_context(units):
    """-> (bar {family: row}, {unit: {family: sites}}) from the real index, or ({}, None) without a field index"""
    import contextlib
    import io
    import qfeed
    import qfeedkey
    import qfeedseen
    import qrendermd
    seen = qfeedseen.open_db()
    W, _ix = qfeedkey.load_index()
    if W is None:
        return {}, None
    with contextlib.redirect_stdout(io.StringIO()):
        rows, _files, _p, _W = qfeed.collect(W)
    bar, sites_t = qrendermd.bar_table(rows, seen)
    return bar, qrendermd.unit_family_sites(sites_t, seen, units)


# ---------------------------------------------------------------- store-system
def _section(line):
    m = re.match(r'^\s*[*`]*(explain|facts|open)[*`]*\s*:\s*(.*)$', line, re.I)
    return (m.group(1).lower(), m.group(2).strip()) if m else (None, None)


def parse_fact(s):
    """'- text | from: unit N | high' -> (text, unit, conf) or None"""
    s = re.sub(r'^[-*•]\s+', '', s.strip())
    parts = [p.strip() for p in s.split('|')]
    if len(parts) < 3:
        return None
    m = re.search(r'from\s*:\s*units?\s*(\d+)', parts[-2], re.I)
    cf = qread.conf_of(parts[-1])
    text = ' | '.join(parts[:-2]).strip()
    if not (m and cf and text):
        return None
    return text, int(m.group(1)), cf


def parse_system_answer(text):
    """-> (blocks, skipped). blocks: dicts system, explain, facts [(text, unit, conf)], bad_facts [line], open [line]. skipped: [(label, why)]"""
    raw, cur = [], None
    for line in text.splitlines():
        h = re.match(r'^\s*#{1,3}\s*(.*?)\s*$', line)
        if h:
            m = re.match(r'system\s*:\s*(\S+)', h.group(1), re.I)
            cur = {'label': h.group(1), 'system': m.group(1).lower().strip('`*.,') if m else None, 'lines': []}
            raw.append(cur)
        elif cur is not None:
            cur['lines'].append(line)
    blocks, skipped = [], []
    for b in raw:
        if b['system'] is None:
            skipped.append((b['label'], 'heading is not "## system: NAME"'))
            continue
        if b['system'] not in qread.SYSTEMS:
            skipped.append((b['label'], 'unknown system name %r' % b['system']))
            continue
        sec, ex, facts, bad, opn = None, [], [], [], []
        for line in b['lines']:
            k, rest = _section(line)
            if k:
                sec = k
                line = rest
                if not line:
                    continue
            s = qread.clean(line) if sec != 'explain' else line.rstrip()
            if sec == 'explain':
                ex.append(s)
            elif sec == 'facts' and s.strip() and not qread.NONE.match(s.strip().lstrip('-* ').strip()):
                f = parse_fact(s)
                (facts.append(f) if f else bad.append(s))
            elif sec == 'open' and s.strip() and not qread.NONE.match(s.strip().lstrip('-* ').strip()):
                opn.append(re.sub(r'^[-*•]\s+', '', s.strip()))
        explain = re.sub(r'\n{3,}', '\n\n', '\n'.join(ex)).strip()
        if not explain:
            skipped.append((b['label'], 'no explain: text'))
            continue
        blocks.append({'system': b['system'], 'explain': explain, 'facts': facts, 'bad_facts': bad, 'open': opn})
    return blocks, skipped


def store_system(text, model='', path=None):
    """-> dict(parsed [systems], skipped, facts, bad_facts)"""
    blocks, skipped = parse_system_answer(text)
    c = qread.open_rw(path)
    now = time.strftime('%Y-%m-%d %H:%M:%S')
    nf = nb = 0
    for b in blocks:
        ex = b['explain']
        if b['open']:
            ex += '\n\n%s\n%s' % (OPEN_HEAD, '\n'.join('- ' + x for x in b['open']))
        c.execute('insert into system_read(system,explain,model,when_) values (?,?,?,?)', (b['system'], ex, model, now))
        c.execute("delete from fact_read where system=? and source='writer'", (b['system'],))     # the facts belong to the newest explanation
        for t, u, cf in b['facts']:
            c.execute('insert into fact_read(text,unit_id,confidence,system,source,when_) values (?,?,?,?,?,?)', (t, u, cf, b['system'], 'writer', now))
            nf += 1
        nb += len(b['bad_facts'])
    c.commit()
    c.close()
    return {'parsed': [b['system'] for b in blocks], 'skipped': skipped, 'facts': nf, 'bad_facts': nb}


# ---------------------------------------------------------------- command line
def main(cmd, rest):
    def opt(flag, default=None):
        return rest[rest.index(flag) + 1] if flag in rest and rest.index(flag) + 1 < len(rest) else default
    pos = [x for i, x in enumerate(rest) if not x.startswith('--') and (i == 0 or not rest[i - 1].startswith('--'))]
    if cmd == 'pick':
        outdir = opt('--out')
        res = pick_reread(int(opt('--n', 30)), outdir)
        for u, b in res:
            print('unit %d  %d B' % (u, b))
        print('reread batch: %d units, %d bytes total%s' % (len(res), sum(b for _u, b in res), ' -> ' + outdir if outdir else ' (no --out: nothing written)'))
        return 0
    if cmd == 'systems':
        c = qread.open_ro()
        if c is None:
            print('nothing stored yet')
            return 0
        st = system_state(c)
        lat = latest_units(c)
        for sy in qread.SYSTEMS:
            e = st.get(sy)
            if not e:
                continue
            w = e['written']
            print('%-9s %3d units  %s' % (sy, e['n'], ('explanation by %s on %s%s' % (w[1], w[2], '  STALE (readings newer: %s)' % e['newest'] if e['stale'] else ''))
                                          if w else 'no explanation written'))
        print('(no system yet): %d units answered before `system:` existed (py tools\\q\\q.py read pick --reread --out DIR)' %
              sum(1 for a in lat.values() if not a['system']))
        return 0
    if cmd == 'system':
        if not pos or pos[0] not in qread.SYSTEMS:
            raise qread.ReadError('q read system NAME [--out FILE]; NAME is one of: %s' % ' | '.join(qread.SYSTEMS))
        name = pos[0]
        c = qread.open_ro()
        if c is None:
            raise qread.ReadError('nothing stored yet')
        import qunits
        units = qunits.open_units()
        bar, counts = real_feed_context(units)
        text = bundle(name, c, units, bar.get(name) if name in FEED_FAMS else None, counts)
        if opt('--out'):
            os.makedirs(os.path.dirname(os.path.abspath(opt('--out'))), exist_ok=True)
            with open(opt('--out'), 'w', encoding='utf-8', newline='\n') as fh:
                fh.write(text)
            print('wrote %s: %d bytes, %d cluster readings' % (opt('--out'), len(text.encode('utf-8')), text.count('\n## unit ') + text.startswith('## unit ')))
        else:
            print(text)
        return 0
    if cmd == 'store-system':
        if not pos:
            raise qread.ReadError('q read store-system ANSWER_FILE [--model NAME]')
        with open(pos[0], encoding='utf-8', errors='replace') as fh:
            r = store_system(fh.read(), opt('--model', ''))
        print('systems stored: %s; skipped %d; facts stored %d (%d fact lines unparsed)' %
              (' '.join(r['parsed']) or 'none', len(r['skipped']), r['facts'], r['bad_facts']))
        for lb, why in r['skipped']:
            print('  SKIPPED "%s": %s' % (lb, why))
        return 0
    raise qread.ReadError('q read: unknown subcommand %s' % cmd)
