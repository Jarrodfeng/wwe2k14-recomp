r"""Self-test of q pairs (S111 CDJOIN; tools/q/qpairs.py). 0 game runs; a synthetic tree in a temp folder.

    py -I tools\q\qpairs_selftest.py

a  a citation in a function body (with its +0x offset) belongs to THAT function, not the one before it; a comment ABOVE a function belongs to the
   function below it; a trailing comment on a constant to the constant; a citation in code is src=code
b  a split part (.inc with a `// split:` parent): the name carries the parent's namespace / type chain, unit = the parent file
c  a db/claims row with `ours` = file:line pairs its game address (+offset) with the function holding that line; a db/seams row pairs the topic's
   game addresses with the function the anchor sits in (src=seam), an address in the row's `why` is src=seam-why
d  q pairs NAME / --game ADDR / --stats / --json answer from the table; the stats count the planted numbers
e  incremental: a second update scans 0 files; one edited file is the only one rescanned and its old pairs are gone
g  offsets: `sub_X +0x1D8`, `sub_X +7F0`, `sub_X+1994`, `sub_X +10` (decimal-looking = hex) are offsets; `0x82000001 +300` (bare after data), `sub_X +add`, `sub_X + 4`
   are not; planted twins drop_decimal and data_bare must each FAIL case g in a subprocess
f  PLANTED TWIN: WWE_QPAIRS_PLANT=wrong_owner attributes each function citation to the function before it; case a must FAIL in a subprocess with it set
r  the relation (tools/q/qpairs_rel.py, S111 PAIRMEAN), on a tree of its own: a REX_HOOK body's citation = WATCHES (hook), the function that
   hook calls = WATCHES (handler), a driver's comment on a function it does not call = WATCHES, a port (computes, never touches the game) =
   MIRRORS (also with a [C probe] evidence tag in its comment), a "logs" comment = WATCHES, an input reader (base) = CITES, a function that
   computes nothing = CITES; q fieldsdiff's pair_rows (the production join) keeps only the MIRRORS function. PLANTED TWIN
   WWE_QPAIRS_PLANT=rel_all_mirrors (the old pairing: every row a mirror) must FAIL case r in a subprocess.
Real line: q pairs --stats on this tree runs (exit 0); FLEADS-B's five settled pairs come out as named (ReadPostSettings vs sub_82C288B0
CITES; motion_fix::Sample vs sub_823D91C8 / sub_823D9608, runin::Trigger vs sub_8259A5A0, OnLightSets vs sub_827F8730, ModelLinkRead vs
sub_82720658 WATCHES), the codediff DIFF's FrontEnd::DrawInstanced vs sub_82519508 stays MIRRORS, Tracker::OnModelCall vs sub_82C04DF0 is not.
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
import qpairs  # noqa: E402

FAILS = []

A_H = """namespace ns {
void Alpha() {
  int x = 1;  // mirrors sub_82000010 +0x1D8
}
// doc: sub_82000020 does this
void Beta() {
  int y = 2;
}
constexpr unsigned kC = 0x82000030;
constexpr unsigned kD = 5;  // game sub_82000040
}
"""
P_CPP = """namespace Q {
struct Binder {
#include "p/part.inc"  // split: part
};
}
"""
PART = """void Member() {
  // sub_82000100
}
"""


REL_H = """namespace r {
REX_HOOK_RAW(sub_82000300) {
  // the original first: sub_82000300 builds the list
  __imp__sub_82000300(ctx, base);
  r::OnDone(ctx.r3.u32);
}
// what sub_82000300 left in its list
inline void OnDone(uint32_t v) { g_seen = v + 1; }
// sub_82000320 doubles the count and adds one
inline int Port(int a) { return a * 2 + 1; }
// sub_82000330's count, logs it once
inline int LogIt(int v) { g_last = v; return v; }
// the word sub_82000340 stored
inline uint32_t Peek(const uint8_t* base, uint32_t a) { return Rd32(base, a + 8); }
// sub_82000350
inline void Nop() { Other(); }
// in the menus' order (sub_82000370)
inline void Drive() { call(sub_82000360, {}); }
// sub_82000380 stops at the key stream's end [C probe]
inline int Ends(int n, int k) { return k >= n ? -1 : k; }
}
"""
REL_WANT = (('r::OnDone', '82000300', 'WATCHES', 'handler'), ('REX_HOOK_RAW(sub_82000300)', '82000300', 'WATCHES', 'hook'),
            ('r::Port', '82000320', 'MIRRORS', 'computes'), ('r::LogIt', '82000330', 'WATCHES', 'words'),
            ('r::Peek', '82000340', 'CITES', 'reader'), ('r::Nop', '82000350', 'CITES', 'no shape'),
            ('r::Drive', '82000370', 'WATCHES', 'hook'), ('r::Ends', '82000380', 'MIRRORS', 'computes'))
REAL_WANT = (('scene_inputs::ReadPostSettings', '82C288B0', 'CITES'), ('wwe2k14::motion_fix::Sample', '823D91C8', 'WATCHES'),
             ('wwe2k14::motion_fix::Sample', '823D9608', 'WATCHES'), ('wwe2k14::runin::Trigger', '8259A5A0', 'WATCHES'),
             ('wwe2k14::scene_live::OnLightSets', '827F8730', 'WATCHES'), ('wwe2k14::wrestlers::ModelLinkRead', '82720658', 'WATCHES'),
             ('rdr_scene::FrontEnd::DrawInstanced', '82519508', 'MIRRORS'))


def case_r():
    """the relation classes on a tree of their own, then q fieldsdiff's pair join over it"""
    with tempfile.TemporaryDirectory() as root:
        put(root, 'src/rel.h', REL_H)
        con, _info = qpairs.update(root)
        try:
            rows = [dict(r) for r in con.execute("SELECT ours, game, rel, why FROM pairs WHERE src = 'comment'")]
        finally:
            con.close()
        for ours, game, rel, why in REL_WANT:
            got = [r for r in rows if r['ours'].endswith(ours) and r['game'] == game]
            check(bool(got) and all(r['rel'] == rel and r['why'].startswith(why) for r in got), 'r %s vs sub_%s = %s (%s)' % (ours, game, rel, why),
                  str(got))
        import qfieldsdiff
        pr = qfieldsdiff.pair_rows(root)
        fns = {fn for (_f, fn) in pr}
        check(fns == {'r::Port', 'r::Ends'}, 'r fieldsdiff pair_rows keeps the MIRRORS functions only', str(sorted(fns)))


def case_real():
    con, _info = qpairs.update(None)
    try:
        for ours, game, rel in REAL_WANT:
            got = [dict(r) for r in con.execute("SELECT rel, why, src, line FROM pairs WHERE ours = ? AND game = ? AND src IN ('comment','code','claim','seam-why')",
                                                (ours, game))]
            check(bool(got) and all(r['rel'] == rel for r in got), 'real %s vs sub_%s = %s' % (ours, game, rel), str(got))
        om = [r['rel'] for r in con.execute("SELECT rel FROM pairs WHERE ours = 'rdr_scene::live::weaponrows::Tracker::OnModelCall' AND game = '82C04DF0'")]
        check(om and 'MIRRORS' not in om, 'real Tracker::OnModelCall vs sub_82C04DF0 is not MIRRORS', str(om))
    finally:
        con.close()


def check(ok, what, detail=''):
    print(('PASS ' if ok else 'FAIL ') + what)
    if not ok:
        FAILS.append(what)
        if detail:
            print('     ' + detail.replace('\n', '\n     ')[:1500])


def put(root, rel, text):
    p = os.path.join(root, *rel.split('/'))
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, 'w', encoding='utf8', newline='') as f:
        f.write(text)


def plant(root):
    put(root, 'src/a.h', A_H)
    put(root, 'src/p.cpp', P_CPP)
    put(root, 'src/p/part.inc', PART)
    put(root, 'db/claims/c.json', json.dumps([
        {'address': '0x82000050', 'function': '0x82000050', 'offset': '0x10', 'mark': 'C', 'claim': 'The claim row.', 'evidence': 'e', 'note': 'n', 'ours': 'src/a.h:3'},
        {'address': '0x82000051', 'function': '0x82000051', 'offset': '0x0', 'mark': 'C', 'claim': 'No ours: not a pair.', 'evidence': 'e', 'note': 'n'}]))
    put(root, 'db/seams/t.json', json.dumps({
        'topic': 't', 'summary': 's', 'open': [],
        'game': [{'addr': '0x82000060', 'what': 'The game row.', 'evidence': 'e', 'mark': 'C', 'source': 's'}],
        'ours': [{'path': 'src/a.h', 'anchor': 'int y = 2;', 'verdict': 'match', 'why': 'as sub_82000070 +0x8 does'}]}))


def has(rows, ours, game, off, src):
    return any(r['ours'].endswith(ours) and r['game'] == game and r['off'] == off and r['src'] == src for r in rows)


def case_a(con):
    alpha = qpairs.by_ours(con, 'Alpha')
    beta = qpairs.by_ours(con, 'Beta')
    kc = qpairs.by_ours(con, 'ns::kC')
    kd = qpairs.by_ours(con, 'kD')
    check(has(alpha, 'ns::Alpha', '82000010', '1D8', 'comment'), 'a1 body citation + offset -> Alpha', str([dict(r) for r in alpha]))
    check(not has(beta, 'ns::Beta', '82000010', '1D8', 'comment'), 'a2 the citation is not Beta\'s')
    check(has(beta, 'ns::Beta', '82000020', '', 'comment'), 'a3 the comment above Beta -> Beta', str([dict(r) for r in beta]))
    check(not has(alpha, 'ns::Alpha', '82000020', '', 'comment'), 'a4 the comment above Beta is not Alpha\'s')
    check(has(kc, 'ns::kC', '82000030', '', 'code'), 'a5 a citation in code -> the constant, src=code', str([dict(r) for r in kc]))
    check(has(kd, 'ns::kD', '82000040', '', 'comment'), 'a6 a trailing comment -> the constant on its line', str([dict(r) for r in kd]))


def case_g():
    t = lambda s: [(a, o) for a, o, _p in qpairs.cites_in(s)]
    check(t('x sub_82000001 +0x1D8 y') == [('82000001', '1D8')], 'g1 sub_ +0x1D8', str(t('x sub_82000001 +0x1D8 y')))
    check(t('sub_82000001 +7F0') == [('82000001', '7F0')], 'g2 sub_ +7F0 (bare hex)')
    check(t('sub_82000001+1994,') == [('82000001', '1994')], 'g3 sub_+1994 (no space)')
    check(t('sub_82000001 +10 [node+4]') == [('82000001', '10')], 'g4 decimal-looking digits are hex')
    check(t('see 0x82000002 +300 and [[0x83929A68]+300]') == [('82000002', ''), ('83929A68', '')], 'g5 no bare offset after a 0x data address', str(t('see 0x82000002 +300 and [[0x83929A68]+300]')))
    check(t('0x82000002 +0x10') == [('82000002', '10')], 'g6 0x data address keeps +0xNN')
    check(t('sub_82000001 +add and sub_82000003 + 4') == [('82000001', ''), ('82000003', '')], 'g7 +word and "+ 4" are not offsets', str(t('sub_82000001 +add and sub_82000003 + 4')))


def run_cases(root):
    con, info = qpairs.update(root)
    try:
        case_a(con)
        case_g()
        if os.environ.get('WWE_QPAIRS_ATTR_ONLY'):
            return info
        part = qpairs.by_ours(con, 'Q::Binder::Member')
        check(has(part, 'Q::Binder::Member', '82000100', '', 'comment') and all(r['unit'] == 'src/p.cpp' for r in part) and part and part[0]['file'] == 'src/p/part.inc',
              'b part file: qualified by the parent chain, unit = the parent', str([dict(r) for r in part]))
        alpha = qpairs.by_ours(con, 'Alpha')
        beta = qpairs.by_ours(con, 'Beta')
        check(has(alpha, 'ns::Alpha', '82000050', '10', 'claim'), 'c1 claim row (function + offset) -> the function holding its ours line', str([dict(r) for r in alpha]))
        check(not qpairs.by_game(con, '82000051')[1], 'c2 a claim with no `ours` is not a pair')
        check(has(beta, 'ns::Beta', '82000060', '', 'seam'), 'c3 seam game row -> the function of the ours anchor', str([dict(r) for r in beta]))
        check(has(beta, 'ns::Beta', '82000070', '8', 'seam-why'), 'c4 address in the row\'s why -> seam-why with its offset')
        a, g = qpairs.by_game(con, 'sub_82000010')
        check(a == '82000010' and len(g) == 1 and g[0]['ours'] == 'ns::Alpha', 'd1 --game: who cites sub_82000010', str([dict(r) for r in g]))
        s = qpairs.stats(con)
        check(s['functions_with_cite'] == 3 and s['our_functions'] == 3 and s['game_distinct'] == 8,
              'd2 stats: 3 of 3 functions cited, 8 distinct game addresses', json.dumps({k: v for k, v in s.items() if k != 'top'}))
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = qpairs.main(['--root', root, 'Alpha', '--json'])
        d = json.loads(buf.getvalue())
        check(rc == 0 and d['n'] == 2 and {r['game'] for r in d['pairs']} == {'82000010', '82000050'}, 'd3 --json of a name', buf.getvalue()[:400])
        return info
    finally:
        con.close()


def main():
    if '--rel-only' in sys.argv:
        case_r()
        return 1 if FAILS else 0
    attr_only = '--attr-only' in sys.argv
    if attr_only:
        os.environ['WWE_QPAIRS_ATTR_ONLY'] = '1'
    with tempfile.TemporaryDirectory() as root:
        plant(root)
        run_cases(root)
        if attr_only:
            return 1 if FAILS else 0
        con, info = qpairs.update(root)
        con.close()
        check(info['scanned'] == 0 and not info['db_rebuilt'], 'e1 second update scans 0 files, rebuilds no db rows', str(info))
        put(root, 'src/a.h', A_H.replace('int y = 2;', 'int y = 2;  // sub_82000200'))
        con, info = qpairs.update(root)
        try:
            rows = qpairs.by_ours(con, 'Beta')
            check(info['scanned'] == 1 and has(rows, 'ns::Beta', '82000200', '', 'comment'), 'e2 one edited file is rescanned, its new pair appears', str(info))
            # the seam anchor line text changed -> that row is stale now: its db pairs go, nothing stale is kept
            check(not has(rows, 'ns::Beta', '82000060', '', 'seam'), 'e3 a seam row whose anchor is gone leaves no pair')
        finally:
            con.close()
        env = dict(os.environ, WWE_QPAIRS_PLANT='wrong_owner')
        p = subprocess.run([sys.executable, '-I', os.path.abspath(__file__), '--attr-only'], capture_output=True, text=True, env=env, timeout=300)
        check(p.returncode != 0 and 'FAIL a3' in p.stdout and 'FAIL a4' in p.stdout, 'f PLANTED TWIN wrong_owner: case a fails in the subprocess', p.stdout[-800:] + p.stderr[-400:])
        for plant_name in ('drop_decimal', 'data_bare'):
            p = subprocess.run([sys.executable, '-I', os.path.abspath(__file__), '--attr-only'], capture_output=True, text=True, timeout=300,
                               env=dict(os.environ, WWE_QPAIRS_PLANT=plant_name))
            check(p.returncode != 0 and 'FAIL g' in p.stdout, 'g PLANTED TWIN %s: case g fails in the subprocess' % plant_name, p.stdout[-800:])
    case_r()
    p = subprocess.run([sys.executable, '-I', os.path.abspath(__file__), '--rel-only'], capture_output=True, text=True, timeout=300,
                       env=dict(os.environ, WWE_QPAIRS_PLANT='rel_all_mirrors'))
    check(p.returncode != 0 and 'FAIL r r::OnDone' in p.stdout and 'FAIL r fieldsdiff' in p.stdout,
          'r PLANTED TWIN rel_all_mirrors: case r fails in the subprocess', p.stdout[-800:])
    case_real()
    p = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'q.py'), 'pairs', '--stats'], capture_output=True, text=True, timeout=600)
    check(p.returncode == 0 and 'our functions with >= 1 citation' in p.stdout, 'real: q pairs --stats on this tree', p.stdout[-600:] + p.stderr[-400:])
    print('%d failed' % len(FAILS) if FAILS else 'ALL PASS')
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
