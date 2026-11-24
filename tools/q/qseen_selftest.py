r"""selftest of qseen: a synthetic coverage folder + a synthetic static site table in a temp folder (no game data).
    py -I tools/q/qseen_selftest.py
The real-index part is SKIPPED when no icalls.tsv exists."""
import os
import sqlite3
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qseen  # noqa: E402


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8') as f:
        f.write(text)


def check(name, ok):
    print('%s %s' % ('ok  ' if ok else 'FAIL', name))
    return ok


def main():
    ok = True
    with tempfile.TemporaryDirectory() as td:
        qseen.COV = os.path.join(td, 'cov')
        qseen.SEEN_DB = os.path.join(td, 'seen.s1.db')
        v = os.path.join(td, 'vcall.s9.db')
        c = sqlite3.connect(v)
        c.execute('create table icall(site integer, func integer, kind text, shape text, off integer, recv text, status text, n integer)')
        c.executemany('insert into icall values (?,?,?,?,?,?,?,?)', [(0x82000104, 0x82000100, 'k', 'V_this', 8, '', 'exact', 1),
                                                                   (0x82000204, 0x82000200, 'k', 'V_this', 8, '', 'cand', 1)])
        c.commit()
        c.close()
        os.environ['WWE_QVCALL_DB'] = v
        hdr = 'func\tlr\ttarget\n'
        # two pairs at one site; one tail row (lr-4 = 0x82000304 is no static site)
        write(os.path.join(qseen.COV, 'r1', 'icalls.tsv'),
              hdr + '0x82000100\t0x82000108\t0x82001000\n0x82000100\t0x82000108\t0x82001010\n0x82000300\t0x82000308\t0x82001000\n')
        con = qseen.open_seen()
        ok &= check('two pairs at one site give two rows', sorted(qseen.targets_of(0x82000104, con)) == [(0x82001000, 1), (0x82001010, 1)])
        cs = qseen.callers_of(0x82001000, con)
        ok &= check('tail row joins on func (site 0)', (0x82000300, 0, 1) in cs and (0x82000100, 0x82000104, 1) in cs)
        ok &= check('sites_seen holds only the static site', qseen.sites_seen(con) == {0x82000104})
        con.close()
        # a second run (a _failed folder) raises runs; a changed set of files rebuilds
        write(os.path.join(qseen.COV, 'r2_failed', 'icalls.tsv'), hdr + '0x82000100\t0x82000108\t0x82001000\n')
        con = qseen.open_seen()
        ok &= check('a second run raises runs', (0x82000100, 0x82000104, 2) in qseen.callers_of(0x82001000, con) and qseen.n_runs(con) == 2)
        k, n = qseen.slot_observed([8], set(), con)
        ok &= check('slot_observed K of N', (k, n) == (1, 2))
        con.close()
    return 0 if ok else 1


def real():
    import importlib
    importlib.reload(qseen)
    if not qseen.tsv_files():
        print('SKIP real index: no icalls.tsv')
        return
    print('real index: %d tsv file(s); run `py tools/q/q.py seen` for the totals' % len(qseen.tsv_files()))


if __name__ == '__main__':
    r = main()
    real()
    print('PASS' if r == 0 else 'FAIL')
    sys.exit(r)
