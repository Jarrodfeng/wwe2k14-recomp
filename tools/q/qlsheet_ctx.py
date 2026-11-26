r"""Context for the listing sheets (S110 LSHEET2), used by qlsheet.py:

  pick_connected(uid, n)     the unit's functions by connectivity: (callers inside the unit + callers outside + observed callers), unnamed
                             first, functions under 40 bytes ignored, ties by size; at most n addresses.

S111 FOLD: the `# NEIGHBOURS` part (the earlier readings of the units around a unit, shown to the reader of the listing sheet) is gone. A
reading took part of its confidence from those lines, so a confidence in the store was not its own evidence; a sheet now carries only what the
tools measured.

Edge count = the number of distinct (calling function, called function) pairs. Every source is opened read-only; nothing is written.
"""
import collections
import glob
import json
import os
import sqlite3
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qnames  # noqa: E402
import qseen  # noqa: E402
import qunits  # noqa: E402

SRC = os.path.dirname(os.path.dirname(HERE))
MIN_BYTES = 40


def ro(path):
    path = qnames.newest(path)      # S110 QREBUILD review: a copy waiting beside a held file is the one to read
    return sqlite3.connect('file:%s?mode=ro' % path.replace('\\', '/'), uri=True)


class Data:
    """the read-only sources, opened once. A test passes its own connections (index, units, seen, names)."""

    def __init__(self, ix=None, un=None, seen=None, nm=None, dbdir=None):
        self.ix = ix or ro(qnames.INDEX_DB)
        self.un = un or ro(qunits.UNITS_DB)
        self.seen = seen if seen is not None else ro(qseen.SEEN_DB)
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
