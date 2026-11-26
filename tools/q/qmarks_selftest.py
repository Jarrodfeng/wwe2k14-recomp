r"""Self-test of q marks (S111 QMARKS; tools/q/qmarks.py). 0 game runs; a PLANTED fixture in a temp folder (claims and seams with all three marks, open items).

    py -I tools\q\qmarks_selftest.py

a  totals first: planted claims C2 / L1 / U1, seam game rows C1 / L1 / U1, 3 open items
b  --mark U prints every [U] row (claim and game row) AND every open item, with the settle sentence read from the text; no L / C row
c  --mark L prints only the L rows and no open item; --seam, --note, --addr filter; --json carries the totals and the rows
d  a long text is cut at TEXT_CAP with the exact call for the rest; --all prints it whole
e  PLANTED TWIN: WWE_QMARKS_PLANT=no_open (open items dropped) must FAIL case a / b; the same cases are run in a subprocess with it set
Real line: q marks --mark U on this tree runs (exit 0, totals first).
"""
import contextlib
import io
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qmarks  # noqa: E402

FAILS = []


def check(ok, what, detail=''):
    print(('PASS ' if ok else 'FAIL ') + what)
    if not ok:
        FAILS.append(what)
        if detail:
            print('     ' + detail.replace('\n', '\n     ')[:1500])


def plant(root):
    os.makedirs(os.path.join(root, 'db', 'claims'))
    os.makedirs(os.path.join(root, 'db', 'seams'))
    long_text = 'The long claim ' + 'x' * 400 + ' [L] one probe settles it.'
    claims = [
        {'address': '0x82000010', 'function': '0x82000010', 'mark': 'C', 'claim': 'Settled claim A.', 'evidence': 'decomp sub_82000010', 'note': 'docs/research/s111/ALPHA.md'},
        {'address': '0x82000020', 'mark': 'C', 'claim': 'Settled claim B.', 'evidence': 'q addr', 'note': 'docs/research/s111/ALPHA.md', 'seam': 'topic_a'},
        {'address': '0x82000030', 'mark': 'L', 'claim': long_text, 'evidence': 'q field 10', 'note': 'docs/research/s111/BETA.md', 'seam': 'topic_a'},
        {'address': None, 'mark': 'U', 'claim': 'Nothing scales the rects [U]: settles: the writers of [[0x83929A68]+300] (q field 300 --writes).', 'evidence': 'q field', 'note': 'docs/research/s111/BETA.md', 'ours': 'src/a.h:5'},
    ]
    seam_a = {'topic': 'topic_a', 'summary': 's', 'ours': [], 'open': ['Open one: whether X holds; one decomp of sub_82000040 settles it.', 'Open two with no way written.'],
              'game': [{'addr': '0x82000050', 'what': 'Game C row.', 'evidence': 'e', 'mark': 'C', 'source': 'docs/research/s111/ALPHA.md'},
                       {'addr': '0x82000060', 'what': 'Game L row.', 'evidence': 'e', 'mark': 'L', 'source': 'docs/research/s111/GAMMA.md'}]}
    seam_b = {'topic': 'topic_b', 'summary': 's', 'ours': [], 'open': ['Open three, settled by reading the table at 0x82000070.'],
              'game': [{'addr': '0x82000080', 'what': 'Game U row [U]: settle by one q words call.', 'evidence': 'e', 'mark': 'U', 'source': 'docs/research/s111/GAMMA.md'}]}
    with open(os.path.join(root, 'db', 'claims', 'c.json'), 'w', encoding='utf8') as f:
        json.dump(claims, f)
    for n, d in (('topic_a', seam_a), ('topic_b', seam_b)):
        with open(os.path.join(root, 'db', 'seams', n + '.json'), 'w', encoding='utf8') as f:
            json.dump(d, f)


def run(root, *argv):
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = qmarks.main(list(argv), root)
    return rc, buf.getvalue()


def cases(root, label=''):
    """the cases whose results depend on the open items; returns the list of failed names (used by the twin)"""
    bad = []
    rc, out = run(root)
    first = out.splitlines()[0] if out else ''
    if not (rc == 0 and first == 'marks: claims C 2 / L 1 / U 1; seam game rows C 1 / L 1 / U 1; 3 open items'):
        bad.append('a totals')
    rc, out = run(root, '--mark', 'U')
    u_ok = (rc == 0 and out.count('[U] claim') == 1 and out.count('[U] game') == 1 and out.count('[open]') == 3
            and '[L]' not in out and '[C]' not in out and 'settles: Nothing' not in out
            and 'settles: the writers of [[0x83929A68]+300]' in out and 'settles: one decomp of sub_82000040 settles it' in out
            and 'settles: settle by one q words call' in out and 'settles: Open one' not in out)
    if not u_ok:
        bad.append('b mark U')
    return bad, out


def main():
    with tempfile.TemporaryDirectory() as root:
        plant(root)
        bad, out = cases(root)
        check('a totals' not in bad, 'a  totals come first and match the planted census')
        check('b mark U' not in bad, 'b  --mark U: every [U] claim and game row, the 3 open items, the settle text, no L / C row', out)
        rc, out = run(root, '--mark', 'L')
        check(rc == 0 and out.count('[L]') == 2 and '[open]' not in out and '[U]' not in out and "settles: (the row names nothing; its evidence: e)" in out,
              'c1 --mark L: the two L rows, no open item, a row with no settle text says so', out)
        rc, out = run(root, '--seam', 'topic_b')
        check(rc == 0 and '[U] game' in out and '[open]' in out and 'Game C row' not in out and 'ALPHA' not in out, 'c2 --seam topic_b filters', out)
        rc, out = run(root, '--note', 'BETA')
        check(rc == 0 and '[L] claim' in out and '[U] claim' in out and 'Settled claim' not in out and 'Game' not in out, 'c3 --note BETA filters', out)
        rc, out = run(root, '--addr', '82000020')
        check(rc == 0 and 'Settled claim B' in out and 'Settled claim A' not in out and '[open]' not in out, 'c4 --addr filters (no open item)', out)
        rc, out = run(root, '--json', '--mark', 'U')
        d = json.loads(out)
        check(rc == 0 and d['shown'] == 5 and d['totals']['open'] == 3 and d['totals']['claim']['U'] == 1, 'c5 --json: totals and rows', out[:600])
        rc, out = run(root, '--mark', 'L', '--seam', 'topic_a')
        check('cut at 240 characters; the full text: q marks --mark L --seam topic_a --all' in out and 'xxxxxxxxxxxxxxxxxxxxxxxx ...' in out, 'd1 long text cut with the call for the rest', out)
        rc, out = run(root, '--mark', 'L', '--seam', 'topic_a', '--all')
        check('x' * 400 in out and 'cut at' not in out, 'd2 --all prints it whole', out)
        rc, out = run(root, '--mark', 'Q')
        check(rc == 2, 'f  a bad --mark exits 2')
        # twin: the open items dropped (in process) must make a / b fail
        qmarks.PLANT = 'no_open'
        try:
            tw = qmarks.collect(root)
            bad2, _ = cases(root)
        finally:
            qmarks.PLANT = ''
        check(len(tw) == 7 and not any(r['kind'] == 'open' for r in tw), 'e1 twin: the planted rule drops the open items')
        check('a totals' in bad2 and 'b mark U' in bad2, 'e2 twin: cases a and b FAIL when the open items are dropped (they would not prove the listing otherwise)', str(bad2))
    # the real line
    r = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'q.py'), 'marks', '--mark', 'U'], capture_output=True, text=True, encoding='utf8', errors='replace')
    first = (r.stdout.splitlines() or [''])[0]
    check(r.returncode == 0 and first.startswith('marks: claims C ') and '[open]' in r.stdout, 'g  real: q marks --mark U on this tree: ' + first, r.stdout[:500] + r.stderr[:500])
    print('selftest: %s' % ('FAIL (%d)' % len(FAILS) if FAILS else 'PASS'))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
