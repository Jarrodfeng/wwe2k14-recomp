r"""Selftest of tools/q/qgamemd.py (the document map): py -I tools\q\qgamemd_selftest.py. No index needed."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qgamemd as G  # noqa: E402
import qread  # noqa: E402
import qrendermd as R  # noqa: E402

FAILS = []


def check(ok, what):
    print('%s  %s' % ('ok  ' if ok else 'FAIL', what))
    if not ok:
        FAILS.append(what)


def fails(fn):
    try:
        fn()
    except SystemExit:
        return True
    return False


def main():
    got = collections_count()
    check(all(got[s] == 1 for s in qread.SYSTEMS), 'every reader system is in exactly one document')
    check(G.doc_of_system('') == 'UNSORTED.md', 'a unit with no system goes to UNSORTED')
    empty = {'answers': {}, 'facts': [], 'sysstate': {}}
    G.validate(empty)
    check(fails(lambda: G.validate({'answers': {1: {'system': 'planted_sys'}}, 'facts': [], 'sysstate': {}})), 'an unmapped system in an answer fails')
    check(fails(lambda: G.validate({'answers': {}, 'facts': [('t', 1, 'high', 'planted_sys', 'reader')], 'sysstate': {}})), 'an unmapped system in a fact fails')
    check(fails(lambda: G.validate({'answers': {}, 'facts': [], 'sysstate': {'planted_sys': {}}})), 'an unmapped system with an explanation fails')
    docs = dict(G.DOCUMENTS)
    docs['PLANTED.md'] = 'planted_sys'
    G.validate({'answers': {1: {'system': 'planted_sys'}}, 'facts': [], 'sysstate': {}}, docs)
    check(True, 'one more line in the table adds a document')
    docs['TWICE.md'] = 'match'
    check(fails(lambda: G.doc_of_system('match', docs)), 'a system in two documents fails')
    facts = [('reader says A', 7, 'high', 'match', 'reader'), ('writer says A again', 7, 'medium', 'match', 'writer'),
             ('another unit', 8, 'high', 'match', 'reader'), ('low one', 9, 'low', 'match', 'writer'), ('menus one', 10, 'high', 'menus', 'reader')]
    out, shown, dropped = R.pick_facts(facts, systems=('match', 'ai'), per_unit=True)
    texts = [t for fl in out.values() for t, _u, _c, _s in fl]
    check(texts == ['writer says A again', 'another unit'] and shown == 2 and dropped == 0, 'one fact per unit, the writer\'s, never a low one, only its systems (%s)' % texts)
    print('%d failed' % len(FAILS) if FAILS else 'all ok')
    return 1 if FAILS else 0


def collections_count():
    import collections
    c = collections.Counter()
    for s in qread.SYSTEMS:
        c[s] += sum(1 for v in G.DOCUMENTS.values() if s in v.split())
    return c


if __name__ == '__main__':
    sys.exit(main())
