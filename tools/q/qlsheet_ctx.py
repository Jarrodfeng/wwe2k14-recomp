r"""Context for the listing sheets (S110 LSHEET2), used by qlsheet.py:

  neighbours(uid)            the units that call INTO a unit and the units it calls OUT to (direct edges + OBSERVED indirect pairs), top 6
                             each way by edge count, each with its newest model reading (system, confidence, purpose cut to 160). A
                             neighbour whose newest purpose is "unknown" or whose confidence is low is skipped unless fewer than 3 would
                             remain (then the best of the skipped fill up to 3); a neighbour with no reading at all is never listed.
  format_neighbours(nb)      the `# NEIGHBOURS` part, one line per neighbour; it says plainly these are earlier readings (leads), not facts.
  pick_connected(uid, n)     the unit's functions by connectivity: (callers inside the unit + callers outside + observed callers), unnamed
                             first, functions under 40 bytes ignored, ties by size; at most n addresses.

Edge count = the number of distinct (calling function, called function) pairs between the two units (a direct pair and an observed pair
of the same two functions count once). Every source is opened read-only; nothing is written.
"""
import collections
import glob
import json
import os
import re
import sqlite3
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qnames  # noqa: E402
import qread  # noqa: E402
import qseen  # noqa: E402
import qunits  # noqa: E402

SRC = os.path.dirname(os.path.dirname(HERE))
TOP = 6
MIN_KEEP = 3
MIN_BYTES = 40
PURPOSE_CUT = 160
LEAD_NOTE = ('# NEIGHBOURS (EARLIER MODEL READINGS of the units around this one: leads, NOT facts; they may be wrong; the sheet and the '
             'listings above and below are what the tool measured)')


def ro(path):
    return sqlite3.connect('file:%s?mode=ro' % path.replace('\\', '/'), uri=True)


class Data:
    """the read-only sources, opened once. A test passes its own connections (index, units, seen, read, names)."""

    def __init__(self, ix=None, un=None, seen=None, rd=None, nm=None, dbdir=None):
        self.ix = ix or ro(qnames.INDEX_DB)
        self.un = un or ro(qunits.UNITS_DB)
        self.seen = seen if seen is not None else ro(qseen.SEEN_DB)
        self.rd = rd if rd is not None else qread.open_ro()
        self.nm = nm if nm is not None else ro(qnames.NAMES_DB)
        self.dbdir = dbdir or os.path.join(SRC, 'db')
        self.fu = dict(self.un.execute('select func,unit_id from units'))
        self._dbnames = None

    def span(self, uid):
        r = self.un.execute('select lo,hi from unit where unit_id=?', (uid,)).fetchone()
        return r

    def db_named(self):
        if self._dbnames is None:
            s = set()
            for p in glob.glob(os.path.join(self.dbdir, '*.json')):
                try:
                    ents = json.load(open(p, encoding='utf-8'))
                except (ValueError, OSError):
                    continue
                for e in ents:
                    try:
                        a = int(e['address'], 16)
                    except (KeyError, ValueError, TypeError):
                        continue
                    if e.get('name'):
                        s.add(a)
            self._dbnames = s
        return self._dbnames

    def reading(self, uid):
        """newest answer of a unit: (system, confidence, purpose) or None"""
        if self.rd is None:
            return None
        r = self.rd.execute('select system,confidence,purpose from unit_read where unit_id=? order by id desc limit 1', (uid,)).fetchone()
        return r


def unit_edges(d, uid):
    """-> (callers {unit: set of (src, dst)}, callees {unit: set of (src, dst)}) over direct + observed pairs"""
    lo, hi = d.span(uid)
    ins, outs = collections.defaultdict(set), collections.defaultdict(set)
    for s, t in d.ix.execute("select src,dst from edges where dst between ? and ? and kind in ('call','tail')", (lo, hi)):
        u = d.fu.get(s)
        if u is not None and u != uid and d.fu.get(t) == uid:
            ins[u].add((s, t))
    for s, t in d.ix.execute("select src,dst from edges where src between ? and ? and kind in ('call','tail')", (lo, hi)):
        u = d.fu.get(t)
        if u is not None and u != uid and d.fu.get(s) == uid:
            outs[u].add((s, t))
    for s, t in d.seen.execute('select func,target from seen where target between ? and ?', (lo, hi)):
        u = d.fu.get(s)
        if u is not None and u != uid and d.fu.get(t) == uid:
            ins[u].add((s, t))
    for s, t in d.seen.execute('select func,target from seen where func between ? and ?', (lo, hi)):
        u = d.fu.get(t)
        if u is not None and u != uid and d.fu.get(s) == uid:
            outs[u].add((s, t))
    return ins, outs


def clean_purpose(p):
    return re.sub(r'\s+', ' ', p or '').strip()


def is_weak(rd):
    return clean_purpose(rd[2]).lower().startswith('unknown') or (rd[1] or '').lower() == 'low' or not clean_purpose(rd[2])


def choose(d, edges):
    """-> [dict(unit, n, system, confidence, purpose)] top TOP by edge count, weak readings only to fill up to MIN_KEEP"""
    rows = []
    for u, pairs in edges.items():
        rd = d.reading(u)
        if rd is not None:
            rows.append((len(pairs), u, rd))
    rows.sort(key=lambda r: (-r[0], r[1]))
    good = [r for r in rows if not is_weak(r[2])]
    if len(good) < MIN_KEEP:
        good += [r for r in rows if is_weak(r[2])][:MIN_KEEP - len(good)]
        good.sort(key=lambda r: (-r[0], r[1]))
    out = []
    for n, u, (sy, cf, pu) in good[:TOP]:
        p = clean_purpose(pu)
        out.append({'unit': u, 'n': n, 'system': sy or '?', 'confidence': cf or '?',
                    'purpose': p if len(p) <= PURPOSE_CUT else p[:PURPOSE_CUT - 1] + '~'})
    return out


def neighbours(uid, d=None):
    """-> {'in': [...], 'out': [...]} (see choose)"""
    d = d or Data()
    ins, outs = unit_edges(d, uid)
    return {'in': choose(d, ins), 'out': choose(d, outs)}


def format_neighbours(nb):
    """the `# NEIGHBOURS` part, or '' when the unit has none to show"""
    lines = []
    for key, verb in (('in', 'called by'), ('out', 'calls')):
        for x in nb[key]:
            lines.append('%s unit %d (x%d) [%s, %s]: %s' % (verb, x['unit'], x['n'], x['system'], x['confidence'], x['purpose']))
    return (LEAD_NOTE + '\n' + '\n'.join(lines) + '\n') if lines else ''


def connectivity(d, uid):
    """-> {func: callers}: distinct direct callers (inside + outside the unit) + distinct observed callers, over the unit's functions"""
    lo, hi = d.span(uid)
    mem = [f for f, u in d.fu.items() if lo <= f <= hi and u == uid]
    ms = set(mem)
    callers = collections.defaultdict(set)
    for s, t in d.ix.execute("select src,dst from edges where dst between ? and ? and kind in ('call','tail')", (lo, hi)):
        if t in ms and s != t:
            callers[t].add(s)
    for s, t in d.seen.execute('select func,target from seen where target between ? and ?', (lo, hi)):
        if t in ms and s != t:
            callers[t].add(s)
    return {f: len(callers[f]) for f in mem}


def pick_connected(uid, n, d=None, named=None):
    """-> [addr] at most n, by (callers desc) with unnamed first, functions under MIN_BYTES ignored, ties by size desc.
    named: a set of addresses to count as named; default = our db / game / index names."""
    d = d or Data()
    lo, hi = d.span(uid)
    conn = connectivity(d, uid)
    if named is None:
        named = set(d.db_named())
        named |= set(f for f, in d.nm.execute("select func from names where source!='fn_string_shared' and func between ? and ?", (lo, hi)))
        named |= set(a for a, in d.ix.execute("select addr from funcs where addr between ? and ? and name is not null and name not like 'sub\\_%' escape '\\'", (lo, hi)))
    rows = []
    for a, size in d.ix.execute('select addr,size from funcs where addr between ? and ?', (lo, hi)):
        if a in conn and size >= MIN_BYTES:
            rows.append((1 if a in named else 0, -conn[a], -size, a))
    return [r[3] for r in sorted(rows)[:n]]
