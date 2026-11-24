r"""Which game functions are generic copiers (memset, memcpy and their like), for the observed-writer join (S110 lane COPIER).

    py -I tools\q\qcopier.py --list        the copiers (and whether lr may be used for each) and the breadth candidates

Why: a store row names the function that stored. For memset / memcpy that is the copier, not the function that wanted the
bytes written, so `q feed` would name sub_82C79530 (memset) as the writer of 78 read sites. The store recorder keeps lr at each
store (src/store_watch.h); in a LEAF function ctx.lr is still the caller's return address, so the join names the function
holding lr - 4 instead (qfeedseen.py). That holds only for a leaf: a copier that calls a helper has overwritten lr, so a copier
with outgoing call edges is NOT attributed through lr (rule c) and stays flagged.

Rules (each a function below):
  a  name_rule         the db name or the game name is memset / memcpy / memmove / memcpy-like           -> a copier
  b  breadth_candidate observed writing read sites in 6 or more different source FILES of ours           -> a CANDIDATE only
                       (counted from the raw store rows by qfeedseen.build, table `breadth`); never a copier unless also (a)
                       or listed in HAND with a reason
  c  leaf_ok           a copier is attributed through lr only if the index has no outgoing call edge for it
Rule 1: addresses and names only; the index lives under out\.
"""
import argparse
import bisect
import os
import re
import sqlite3
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MAIN = r'C:\Users\Jarrod\wwe2k14-recomp'
INDEX_DB = os.path.join(MAIN, 'out', 'harness', 'index', 'index.db')
BREADTH_FILES = 6

# rule a: exact words (after stripping leading underscores, lower case) ...
NAME_WORDS = {'memset', 'memcpy', 'memmove', 'bzero', 'copy_bytes', 'copy_words'}
# ... and the memcpy-like families: memcpy_aligned, strcpy_s, memset32, __memmove_fast, memset_vmx. A bare ^(mem|str)(cpy|set|move)
# prefix also takes game functions the db named by what they do (memset_zero_56_records_of_12_bytes_stride_88: a specific
# writer, not a copier), so after the word only a short generic suffix is allowed.
NAME_RE = re.compile(r'^(mem|str)(cpy|set|move)(_(s|forward|backward|fast|aligned|vmx)|\d+)?$')

# Hand list: {function address: why}. A function that is a copier by what it does but not by its name. Empty on purpose: a
# breadth candidate (rule b) is added here only after someone read the function; one comment per entry.
HAND = {}


def name_rule(name):
    """rule a: True when `name` is a known copier name (leading underscores and a trailing ` [..]` note ignored)"""
    if not name:
        return False
    n = name.strip().lower().lstrip('_')
    return n in NAME_WORDS or bool(NAME_RE.match(n))


def breadth_candidate(n_files):
    """rule b: True when a function was seen writing read sites in enough different source files"""
    return n_files >= BREADTH_FILES


class Index:
    """the functions of the index (address ranges, game names) and their outgoing call edges"""

    def __init__(self, funcs, edge_srcs=None, con=None, stat=''):
        funcs = sorted(funcs)
        self.addrs = [f[0] for f in funcs]
        self.sizes = [f[1] for f in funcs]
        self.names = {f[0]: f[2] for f in funcs}
        self.edge_srcs = edge_srcs     # a set of sources (tests) ...
        self.con = con                 # ... or the live db
        self.stat = stat

    @classmethod
    def open(cls, path=None):
        sys.path.insert(0, HERE)
        import qpublish
        path = qpublish.newest(path or INDEX_DB)      # S110 QREBUILD review: a copy waiting beside a held index.db is the one to read
        if not os.path.exists(path):
            return None
        con = sqlite3.connect('file:%s?mode=ro' % path.replace('\\', '/'), uri=True)
        try:
            rows = [(a, s or 0, n) for a, n, s in con.execute('select addr, name, size from funcs')]
        except sqlite3.Error:
            con.close()
            return None
        st = os.stat(path)
        return cls(rows, con=con, stat='%d:%d' % (int(st.st_mtime), st.st_size))

    def func_of(self, a):
        """the function whose range holds address a, or 0"""
        i = bisect.bisect_right(self.addrs, a) - 1
        if i >= 0 and a < self.addrs[i] + max(self.sizes[i], 4):
            return self.addrs[i]
        return 0

    def n_out_edges(self, f):
        if self.edge_srcs is not None:
            return 1 if f in self.edge_srcs else 0
        return self.con.execute('select count(*) from edges where src=?', (f,)).fetchone()[0]


def db_names():
    """{addr: name} from the function db (tools/q/q.py load_db_entries); {} when that cannot be loaded"""
    try:
        if HERE not in sys.path:
            sys.path.insert(0, HERE)
        import q as qmod
        return {addr: e.get('name') for addr, _f, e in qmod.load_db_entries() if e.get('name')}
    except Exception:
        return {}


def game_names(index):
    """{addr: name} of index functions that carry a game name (not the sub_XXXXXXXX placeholder)"""
    return {a: n for a, n in index.names.items() if n and not re.match(r'^(__imp__)?sub_[0-9A-Fa-f]{8}$', n)} if index else {}


def leaf_ok(index, f):
    """rule c: (True, '') when the index shows no outgoing call edge for f; else (False, why). lr is the caller's return
    address only in a function that made no call."""
    if index is None:
        return False, 'no index: leaf not verified, lr not used'
    n = index.n_out_edges(f)
    if n:
        return False, 'has outgoing call edges: lr is its callee\'s return address, not used'
    return True, ''


class Copiers:
    """the copier set: .copiers {addr: {'reason', 'attrib', 'note'}}, .index, .stamp (changes when the set or the index does)"""

    def __init__(self, copiers, index=None):
        self.copiers = copiers
        self.index = index
        self.stamp = '%s|%s' % (index.stat if index else 'noindex',
                                ','.join('%08X%s' % (a, 'L' if c['attrib'] else 'n') for a, c in sorted(copiers.items())))

    def is_copier(self, f):
        return f in self.copiers

    def attrib(self, f):
        """True when a store row of copier f with an lr may be attributed to the caller"""
        c = self.copiers.get(f)
        return bool(c and c['attrib'])


def compute(names, index):
    """names: {addr: name} (db names and game names merged by the caller) -> Copiers"""
    out = {}
    for a, n in names.items():
        if name_rule(n):
            out[a] = {'reason': 'name %s' % n}
    for a, why in HAND.items():
        out.setdefault(a, {'reason': 'hand list: %s' % why})
    for a, c in out.items():
        ok, why = leaf_ok(index, a)
        c['attrib'] = ok
        c['note'] = why
    return Copiers(out, index)


def default():
    """the live copier set: db names + game names of the index; (nothing known when there is no index)"""
    index = Index.open()
    names = dict(game_names(index))
    names.update(db_names())
    return compute(names, index)


def main(argv):
    ap = argparse.ArgumentParser(prog='qcopier')
    ap.add_argument('--list', action='store_true', help='print the copiers and the breadth candidates')
    a = ap.parse_args(argv)
    if not a.list:
        ap.print_usage()
        return 2
    cop = default()
    names = dict(game_names(cop.index))
    names.update(db_names())
    print('copiers: %d (rule a: name; c: lr only for a leaf)' % len(cop.copiers))
    for f, c in sorted(cop.copiers.items()):
        print('  sub_%08X  %-14s %s  %s' % (f, names.get(f, ''), 'lr used' if c['attrib'] else 'lr NOT used', c['reason'] + ('; ' + c['note'] if c['note'] else '')))
    try:
        sys.path.insert(0, HERE)
        import qfeedseen
        con = qfeedseen.open_db()
        rows = qfeedseen.breadth_rows(con)
    except Exception as e:
        print('candidates: not known (%s)' % e)
        return 0
    cand = [(n, f, ns) for f, n, ns in rows if breadth_candidate(n) and f not in cop.copiers]
    print('candidates (rule b: writing read sites in >= %d source files, not copiers; NOT used as copiers): %d' % (BREADTH_FILES, len(cand)))
    for n, f, ns in sorted(cand, reverse=True):
        print('  sub_%08X  %-14s %d files, %d sites' % (f, names.get(f, ''), n, ns))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
