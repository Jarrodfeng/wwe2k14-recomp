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
import sqlite3
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


def members(c):
    """{system: {unit: via}} (S111 READLINK): the units a system's bundle (`q read system NAME --from ..`) reached, recorded when the bundle was made.
    A unit belongs to every system whose bundle reached it, whatever its reading's own `system` field says. {} on a store made before the table."""
    out = {}
    try:
        for sy, u, via in c.execute('select system,unit_id,via from system_unit order by rowid'):
            out.setdefault(sy, {})[u] = via or ''
    except sqlite3.OperationalError:
        pass
    return out


def record_members(system, sel, path=None):
    """store the reach set of a bundle as the members of SYSTEM (the newest bundle replaces the older one). sel: [(unit, via text)]"""
    c = qread.open_rw(path)
    now = time.strftime('%Y-%m-%d %H:%M:%S')
    c.execute('delete from system_unit where system=?', (system,))
    c.executemany('insert or replace into system_unit(system,unit_id,via,when_) values (?,?,?,?)', [(system, u, via, now) for u, via in sel])
    c.commit()
    c.close()


def belonging(c, lat=None):
    """{system: set of units with a stored reading that belong to it}: by the reading's own `system` field, plus the units its bundle reached"""
    lat = latest_units(c) if lat is None else lat
    out = {}
    for u, a in lat.items():
        if a['system']:
            out.setdefault(a['system'], set()).add(u)
    for sy, us in members(c).items():
        for u in us:
            if u in lat:
                out.setdefault(sy, set()).add(u)
    return out


def system_state(c):
    """{system: {'n': units, 'newest': date of the newest reading OF THOSE UNITS, 'written': (explain, model, when) or None, 'stale': bool}}
    for every system with a reading or an explanation; '' (no system yet) is not a system. STALE = a reading of one of this system's own
    units is newer than its explanation (a newer reading of an unrelated unit does not count)."""
    lat = latest_units(c)
    out = {}
    for sy, us in belonging(c, lat).items():
        out[sy] = {'n': len(us), 'newest': max(lat[u]['when'] for u in us), 'written': None, 'stale': False}
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
            text, fl = qsheet.build(ctx, u, earlier=False)       # a re-read is a new reading, not shown the earlier one (S111 FOLD)
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
def bundle(system, c, units, bar_row=None, writer_counts=None, sel=None, maxn=250, skip=0, rest_call=''):
    """the text a system-writer reads. units: a units.s1.db connection (ranges); bar_row: [sites, reached, static, observed, either,
    copier_only, unknown] or None; writer_counts: {unit: {family: read sites its functions were seen writing}} or None.
    sel (S111 PICKPATH): [(unit, 'depth D via sub_P')] in reach order = the selection is the reach set: every unit of it with a stored
    reading, whatever its `system` field. Units past skip+maxn are listed by id only, with rest_call (the call for the rest)."""
    allv = latest_units(c)
    if sel is None:
        mem = members(c).get(system, {})
        lat = {u: a for u, a in allv.items() if a['system'] == system or u in mem}       # its own field, or reached by its bundle (S111 READLINK)
        order, how = sorted(lat), {u: v for u, v in mem.items() if u in lat}
    else:
        order = [u for u, _h in sel if u in allv]
        how = dict(sel)
        lat = {u: allv[u] for u in order}
    cut = order[skip + maxn:] if sel is not None else []
    order = order[skip:skip + maxn] if sel is not None else order
    o = ['SYSTEM: %s' % system,
         '%d cluster readings stored; newest %s. Every reading below is a model reader\'s reading of a tool-made fact sheet: a LEAD, not a fact.'
         % (len(lat), max([a['when'] for a in lat.values()] or ['-'])),
         '']
    if sel is not None:
        o.append('SELECTION: the %d units of the reach set (%d units reached) that have a stored reading, in reach order; the `system` field of a reading is not used.'
                 % (len(lat), len(sel)))
        o.append('')
    if bar_row:
        o.append('FEED BAR for this family: %d read sites in our renderer, %d reached in recorded runs; of those, writer known by static tables %d, '
                 'observed %d, either %d, copier only %d, no known writer %d.' % tuple(bar_row))
        o.append('')
    for u in order:
        a = lat[u]
        rng = ''
        r = units.execute('select lo,hi,n_funcs from unit where unit_id=?', (u,)).fetchone() if units is not None else None
        if r:
            rng = ', 0x%08X..0x%08X, %d funcs' % r
        o.append('## unit %d%s%s' % (u, rng, ('  ' + how[u]) if u in how else ''))
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
    if cut:
        o.append('NOT SHOWN (--max %d): %d more units with a stored reading, by id: %s' % (maxn, len(cut), ' '.join(str(u) for u in cut)))
        o.append('the rest: %s' % rest_call)
        o.append('')
    if not lat:
        o.append('(no stored reading names this system yet)' if sel is None else '(no unit of the reach set has a stored reading)')
    return '\n'.join(o) + '\n'


def bar_table(rows, seen):
    """per family and ALL: [sites, reached, static, observed, either, copier_only, unknown] (the q feed OBSERVED table)"""
    import qfeedseen
    sites_t = {}
    for r in rows:
        if r['kind'] == 'reg':
            continue
        e = sites_t.setdefault((r['file'], r['line']), [r['family'], False])
        e[1] = e[1] or r.get('result') in ('known', 'image')
    reached = qfeedseen.sites(seen)
    named = qfeedseen.named_sites(seen)
    co_only = qfeedseen.copier_only_sites(seen)
    out = {}
    for f in list(FEED_FAMS) + ['ALL']:
        ks = [k for k, v in sites_t.items() if f == 'ALL' or v[0] == f]
        rc = [k for k in ks if k in reached]
        st = sum(1 for k in rc if sites_t[k][1])
        ob = sum(1 for k in rc if k in named)
        ei = sum(1 for k in rc if sites_t[k][1] or k in named)
        co = sum(1 for k in rc if k in co_only and not (sites_t[k][1] or k in named))
        out[f] = [len(ks), len(rc), st, ob, ei, co, len(rc) - ei - co]
    return out, sites_t


def unit_family_sites(sites_t, seen, units):
    """{unit: {family: read sites its functions were seen writing}} over every unit (any size)"""
    uof = dict(units.execute('select func,unit_id from units'))
    acc = collections.defaultdict(lambda: collections.defaultdict(set))
    for fl, ln, fn in seen.execute('select file,line,func from writers where func<>0 and nocaller=0'):
        k = (fl, ln)
        u = uof.get(fn)
        if k in sites_t and u is not None:
            acc[u][sites_t[k][0]].add(k)
    return {u: {f: len(s) for f, s in d.items()} for u, d in acc.items()}


def real_feed_context(units):
    """-> (bar {family: row}, {unit: {family: sites}}) from the real index, or ({}, None) without a field index"""
    import contextlib
    import io
    import qfeed
    import qfeedkey
    import qfeedseen
    seen = qfeedseen.open_db()
    W, _ix = qfeedkey.load_index()
    if W is None:
        return {}, None
    with contextlib.redirect_stdout(io.StringIO()):
        rows, _files, _p, _W = qfeed.collect(W)
    bar, sites_t = bar_table(rows, seen)
    return bar, unit_family_sites(sites_t, seen, units)


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
        if not re.fullmatch(r'[a-z0-9_]+', b['system']):          # any [a-z0-9_]+ name (a reach-set page: create_moveset), not only qread.SYSTEMS
            skipped.append((b['label'], 'system name %r is not [a-z0-9_]+' % b['system']))
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


# ---------------------------------------------------------------- the stored page (S111 PAGEREAD)
def stored_names(c):
    return sorted(r[0] for r in c.execute('select distinct system from system_read'))


def stored_page(c, name):
    """the newest stored page of NAME (explain + open lines as stored, then the writer facts) or None"""
    r = c.execute('select explain,model,when_ from system_read where system=? order by id desc limit 1', (name,)).fetchone()
    if r is None:
        return None
    o = ['STORED PAGE: %s   (written by %s on %s; a model reader\'s reading: a LEAD, not a fact)' % (name, r[1] or '?', r[2] or '?'), '', r[0], '']
    facts = list(c.execute("select text,unit_id,confidence from fact_read where system=? and source='writer' order by id", (name,)))
    o.append('curiosities (%d):' % len(facts))
    o.extend('- %s | from: unit %s | %s' % f for f in facts)
    return '\n'.join(o) + '\n'


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
        for sy in list(qread.SYSTEMS) + sorted(s for s in st if s not in qread.SYSTEMS):
            e = st.get(sy)
            if not e:
                continue
            w = e['written']
            print('%-9s %3d units  %s' % (sy, e['n'], ('explanation by %s on %s%s' % (w[1] if w[1] != '?' else '? (store-system ran without --model)', w[2],
                                                                                    '  STALE (a reading of its own units is newer: %s)' % e['newest'] if e['stale'] else ''))
                                          if w else 'no explanation written'))
        owned = set().union(*belonging(c, lat).values()) if st else set()
        print('(no system yet): %d units answered before `system:` existed and reached by no bundle (py tools\\q\\q.py read pick --reread --out DIR)' %
              sum(1 for u, a in lat.items() if not a['system'] and u not in owned))
        return 0
    if cmd == 'system':
        frm = opt('--from')
        cof = opt('--callers-of')
        if not pos or not re.fullmatch(r'[a-z0-9_]+', pos[0]):
            raise qread.ReadError('q read system NAME [--out FILE]; NAME is one of: %s, or any stored page (q read systems)\n  or q read system NAME --from ENTRY[,ENTRY..] [--depth D] [--k K] [--classes on|off] [--middleware on|off] [--max N] [--skip N]'
                                  ' (NAME any [a-z0-9_]+; the bundle is the reach set, whatever the readings\' system field says)' % ' | '.join(qread.SYSTEMS))
        name = pos[0]
        c = qread.open_ro()
        if c is None:
            raise qread.ReadError('nothing stored yet')
        page = None
        if frm is None and cof is None:
            page = stored_page(c, name)
            if page is None and name not in qread.SYSTEMS:
                raise qread.ReadError('q read system %s: no stored page and not a fixed system name (%s)\n  stored pages: %s\n  (q read systems lists them; or q read system %s --from ENTRY[,ENTRY..])'
                                      % (name, ' | '.join(qread.SYSTEMS), ' '.join(stored_names(c)) or 'none', name))
            if page is not None and name not in qread.SYSTEMS and name not in members(c):
                print(page)       # a stored non-fixed name: the page only (a fixed name keeps its old bundle, page first)
                return 0
        import qunits
        units = qunits.open_units()
        if frm is not None or cof is not None:
            import qreadfrom
            depth, k, cls = int(opt('--depth', qreadfrom.DEPTH if frm is not None else 1)), int(opt('--k', qreadfrom.LIB_K)), opt('--classes', 'on')
            maxn, skip, mw = int(opt('--max', 250)), int(opt('--skip', 0)), opt('--middleware', 'on')
            _ctx, g, _ents, res = qreadfrom.reach_set(qreadfrom.parse_entries(frm) if frm is not None else [], depth, k, cls, middleware=mw,
                                                        callers_of=qreadfrom.parse_entries(cof) if cof is not None else ())
            sel = [(u, qreadfrom.via_text(res, u, d)) for u, d in res['units']]
            call = 'py tools\\q\\q.py read system %s %s --depth %d --k %d --classes %s --middleware %s --skip %d --max %d' % (
                name, ' '.join(x for x in (('--from ' + frm) if frm else '', ('--callers-of ' + cof) if cof else '') if x), depth, k, cls, mw, skip + maxn, maxn)
            text = bundle(name, c, units, None, None, sel, maxn, skip, call)
            record_members(name, sel)       # the reach set is the membership: `q read systems` counts these units under NAME
        else:
            bar, counts = real_feed_context(units)
            text = bundle(name, c, units, bar.get(name) if name in FEED_FAMS else None, counts)
        if opt('--out'):
            os.makedirs(os.path.dirname(os.path.abspath(opt('--out'))), exist_ok=True)
            with open(opt('--out'), 'w', encoding='utf-8', newline='\n') as fh:
                fh.write(text)
            print('wrote %s: %d bytes, %d cluster readings' % (opt('--out'), len(text.encode('utf-8')), text.count('\n## unit ') + text.startswith('## unit ')))
        else:
            print((page + '\n' + text) if page else text)
        return 0
    if cmd == 'store-system':
        if not pos:
            raise qread.ReadError('q read store-system ANSWER_FILE [--model NAME]')
        with open(pos[0], encoding='utf-8', errors='replace') as fh:
            r = store_system(fh.read(), opt('--model', ''))
        if not opt('--model'):
            print('WARNING: no --model given: the explanation is stored with author "?" (pass --model NAME so `q read systems` can say who wrote it)')
        print('systems stored: %s; skipped %d; facts stored %d (%d fact lines unparsed)' %
              (' '.join(r['parsed']) or 'none', len(r['skipped']), r['facts'], r['bad_facts']))
        for lb, why in r['skipped']:
            print('  SKIPPED "%s": %s' % (lb, why))
        return 0
    raise qread.ReadError('q read: unknown subcommand %s' % cmd)
