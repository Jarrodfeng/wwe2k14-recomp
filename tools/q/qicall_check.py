r"""qicall_check: the game judges the static indirect-call rules (S110 CLASSFLOW review). Joins the (site, target) pairs a coverage run recorded against the
vcall db's answers: an observed target outside an exact / narrow site's set is a wrong answer that read as certain.
    py -I tools\q\qicall_check.py out\harness\coverage\NNN\icalls.tsv [--db PATH] [--all]
TSV columns: func, lr, target (hex); the call site is lr - 4. Counted per site (a site contradicts when ANY observed target is outside its set), per status
and per rule (icall_target.how). Exit 1 on a contradiction of an exact or narrow site; a cand site that misses is printed but does not fail (it is a candidate).
"""
import collections
import os
import sqlite3
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SURE = ('exact', 'narrow')


def read_tsv(path):
    """-> [(func, lr, target)]"""
    out = []
    with open(path, encoding='utf-8') as f:
        for ln in f:
            p = ln.split()
            if len(p) < 3 or not p[1].lower().startswith('0x'):
                continue
            out.append((int(p[0], 16), int(p[1], 16), int(p[2], 16)))
    return out


def attribute(raw, rows):
    """-> ({site: observed targets}, n tail rows given to the one tail site of their function, n rows not placed). The recorder writes (func, ctx.lr, target)
    (src/icall_observe.h:10): at a bctrl lr - 4 is the site, in func; at a tail bctr lr is the CALLER's return address, so the row belongs to a bctr of func,
    not to the site at lr - 4 (that site's own target only tail-branched there). A tail row goes to func's tail site when it has exactly one."""
    tails = collections.defaultdict(list)
    for site, (func, kind, shape, status) in rows.items():
        if kind != 'call':
            tails[func].append(site)
    obs, nt, lost = collections.defaultdict(set), 0, 0
    for func, lr, t in raw:
        r = rows.get(lr - 4)
        if r is not None and r[0] == func and r[1] == 'call':
            obs[lr - 4].add(t)
        elif len(tails.get(func, ())) == 1:
            obs[tails[func][0]].add(t)
            nt += 1
        else:
            lost += 1
    return obs, nt, lost


def check(raw, db):
    """-> (counts {(status, how): [agree, contradict]}, contradictions [(site, func, status, how, static set, observed outside)], tail rows placed, rows not placed)"""
    v = sqlite3.connect(f'file:{db}?mode=ro', uri=True)
    rows = {r[0]: r[1:] for r in v.execute('SELECT site, func, kind, shape, status FROM icall')}
    obs, nt, lost = attribute(raw, rows)
    tg = collections.defaultdict(set)
    how = {}
    for site, t, h in v.execute('SELECT site, target, how FROM icall_target'):
        tg[site].add(t)
        how[site] = h
    v.close()
    counts = collections.defaultdict(lambda: [0, 0])
    bad = []
    for site, seen in sorted(obs.items()):
        func, kind, shape, status = rows[site]
        h = how.get(site, '-')
        if status not in SURE + ('cand',):
            counts[(status, '-')][0] += 1      # offset / open: no set is stored, nothing to contradict
            continue
        out = seen - tg[site]
        counts[(status, h)][1 if out else 0] += 1
        if out:
            bad.append((site, func, status, h, sorted(tg[site]), sorted(out)))
    return counts, bad, obs, nt, lost


def main(argv):
    if not argv or argv[0].startswith('-'):
        print(__doc__)
        return 2
    db = argv[argv.index('--db') + 1] if '--db' in argv else None
    if db is None:
        sys.path.insert(0, HERE)
        import qx7
        db = os.environ.get('WWE_QVCALL_DB') or qx7.vdb_path()
    raw = read_tsv(argv[0])
    counts, bad, obs, nt, lost = check(raw, db)
    print(f'{db}: {len(raw)} rows -> {len(obs)} sites; {nt} tail-bctr rows given to the one tail site of their function; {lost} rows not placed '
          f'(a tail row of a function with 0 or 2+ tail sites, or no icall row at lr - 4)')
    print(f'  {"status":<8} {"how":<16} {"agree":>6} {"contradict":>10}')
    for (st, h), (ag, co) in sorted(counts.items(), key=lambda x: (x[0][0] not in SURE, x[0])):
        print(f'  {st:<8} {h:<16} {ag:>6} {co:>10}')
    for site, func, st, h, s, out in bad:
        print(f'  CONTRADICT {st} [{h}] site 0x{site:08X} in sub_{func:08X}: static {", ".join(f"0x{x:08X}" for x in s[:8])}'
              f'{" ..." if len(s) > 8 else ""}; observed outside {", ".join(f"0x{x:08X}" for x in out)}')
    nsure = sum(1 for b in bad if b[2] in SURE)
    print(f'{"FAIL" if nsure else "OK"}: {nsure} exact / narrow sites contradicted by the run, {len(bad) - nsure} candidates missed')
    return 1 if nsure else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
