r"""Selftest of tools/q/qgx.py + qstores.py (S105 lane QG): each subcommand re-finds a real answer from the player's own files / the index and
rejects a planted twin (a synthetic fixture with one deliberate wrong value). Real-data checks skip (SKIP line) when game_data / the
index are missing; the synthetic ones always run. py -I tools\q\qgx_selftest.py"""
import hashlib
import os
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qgx          # noqa: E402
import qstores      # noqa: E402

FAILS = []


def check(name, ok, detail=''):
    print(('ok   ' if ok else 'FAIL ') + name + ((' ' + detail) if detail and not ok else ''))
    if not ok:
        FAILS.append(name)


def fp(t):
    """(sha1[:12], length) of a game text: a check asserts this, never the text itself (rule 1: nothing of the game's text is committed)."""
    return (hashlib.sha1(t.encode()).hexdigest()[:12], len(t))


def skip(name, why):
    print('SKIP %s (%s)' % (name, why))


# ------------------------------------------------------------------------------------------ synthetic twins
def twin_text():
    ids = [(7, 'ONE'), (9, 'NINE')]
    strs = b''
    recs = b''
    base = 8 + 12 * len(ids)
    for tid, t in ids:
        b = t.encode() + b'\0'
        recs += struct.pack('<III', base + len(strs), len(b), tid)
        strs += b
    blob = struct.pack('<II', 0, len(ids)) + recs + strs
    t = qgx.parse_text_table(blob)
    check('text twin: id 9 -> NINE', t.get(9) == 'NINE')
    check('text twin: planted id 8 is absent', 8 not in t)
    check('text twin: a table with a bad header is refused', qgx.parse_text_table(struct.pack('<II', 1, 2) + b'\0' * 40) == {})


def twin_csdt():
    def rec(key, st, en, text):
        b = text.encode() + b'\0'
        b += b'\0' * ((-len(b)) % 4)
        return struct.pack('<IIHHHH', key, 0, st, en, len(b), 1) + b
    blob = struct.pack('<IIIHH', 0, 0, 0, 4, 1) + b'\0' * 4          # header record: key 0, length 4 (the real tables start so)
    blob += rec(10, 128, 402, 'HELLO') + rec(20, 402, 534, 'WORLD') + struct.pack('<I', 0xFFFFFFFF) + b'\0' * 12 + rec(30, 1, 2, 'AFTER-SENTINEL')
    r = qgx.csdt_records(blob)
    check('csdt twin: 2 records before the -1 sentinel', len(r) == 2, str(len(r)))
    check('csdt twin: frames are the u16 values doubled', r and (r[0]['start'], r[0]['end']) == (256, 804))
    check('csdt twin: planted key 30 after the sentinel is not read', all(x['key'] != 30 for x in r))


class FakeCon:
    def execute(self, q, args=()):
        class R:
            def fetchone(s):
                return None
        return R()


def twin_switch():
    lines = ['\t// cmplwi cr6,r11,2', '\t// bgt cr6,0x82000100', '\t// bctr ', '\tswitch (ctx.r11.u32) {', '\tcase 0:', '\t\tgoto loc_82000010;',
             '\tcase 1:', '\t\tgoto loc_82000020;', '\tcase 2:', '\t\tgoto loc_82000020;', '\t}',
             'loc_82000010:', '\t// li r3,5', '\t// li r4,-1', '\t// stw r3,8(r31)', '\t// b 0x82000200',
             'loc_82000020:', '\t// lis r11,2', '\t// ori r11,r11,4', '\t// bl 0x82000300', '\t// blr']
    sws, labels = qgx.parse_switches(lines)
    check('switch twin: one switch, 3 cases, bound 2, default 82000100', len(sws) == 1 and len(sws[0]['cases']) == 3 and sws[0]['range'] == 2 and sws[0]['default'] == 0x82000100, str(sws))
    t = {t for _, t in sws[0]['cases']}
    b0 = qgx.case_body(lines, labels, labels[0x82000010] + 1, t - {0x82000010}, FakeCon(), {})
    check('switch twin: case 0 li r3=5, r4=-1, stores 5 into 8(r31)', (3, 5) in b0['consts'] and (4, 0xFFFFFFFF) in b0['consts'] and b0['stores'] == [('stw', 5, '8(r31)')], str(b0))
    b1 = qgx.case_body(lines, labels, labels[0x82000020] + 1, t - {0x82000020}, FakeCon(), {})
    check('switch twin: cases 1,2 share a target; lis+ori = 0x20004 reaches the bl', (11, 0x20004) in b1['consts'] and b1['calls'][0][0] == 'sub_82000300', str(b1))
    check('switch twin: planted wrong constant (3, 6) is NOT reported', (3, 6) not in b0['consts'])


def twin_strip_flags():
    rest = qgx.qfield_args(['0x10', '--stores', '--var-span', '4096', '--all-copies', '--writes', '--global', '0x83930050'])
    check('strip twin: --var-span N and --all-copies reach qstores, not qfield', rest == ['0x10', '--writes', '--global', '0x83930050'], str(rest))
    rest = qgx.qfield_args(['0x10', '--stores', '--var-span=64'])
    check('strip twin: --var-span=N form and the default --writes', rest == ['0x10', '--writes'], str(rest))
    h = qstores.none_hint('0x836F38D0', 0x836F38D0)
    check('stores twin: an empty --global answer says what --global means and the right spelling', 'pointer stored at' in h and 'field 0x836F38D0 --stores' in h, h)
    check('stores twin: no hint without --global', qstores.none_hint(None, 0x10) == '')


def twin_stores():
    rows, calls = [], []
    SELF = 0x82000000
    st = {3: ('a0', 0), 9: ('K', 15229), 4: ('K', 1)}
    qstores.collect(rows, calls, st, [0x1000, 'stbx', 'r4,r3,r9', [], False], SELF)
    check('stores twin: stbx r4,r3,r9 with r9=15229 -> a0 +15229', rows == [(SELF, 0x1000, 'stbx', 1, 'a0', 15229, 0, 0)], str(rows))
    rows.clear()
    qstores.collect(rows, calls, st, [0x1004, 'stwx', 'r4,r3,r8', [], False], SELF)
    check('stores twin: unknown index register = variable index from a0 +0', rows and rows[0][5:7] == (0, 1), str(rows))
    rows.clear()
    qstores.collect(rows, calls, {3: ('s', 0), 9: ('K', 5)}, [0x1008, 'stbx', 'r4,r3,r9', [], False], SELF)
    check('stores twin: stack-root stores are dropped', rows == [])
    # (M1) both sides known and rooted: one var row per side
    rows.clear()
    qstores.collect(rows, calls, {3: ('a0', 0), 4: ('p(a0,8)', 0)}, [0x1100, 'stbx', 'r5,r3,r4', [], False], SELF)
    check('stores twin (M1): both-rooted index = a var row per side', sorted((r[4], r[6]) for r in rows) == [('a0', 1), ('p(a0,8)', 1)], str(rows))
    # (M2) a global array indexed by a variable: the global is the base, var = 1
    rows.clear()
    qstores.collect(rows, calls, {11: ('K', 0x8376AAF4), 3: ('a0', 0)}, [0x1104, 'stbx', 'r5,r3,r11', [], False], SELF)
    check('stores twin (M2): lis/addi global + a0 index -> root K off 0x8376AAF4 var 1', [(r[4], r[5], r[6]) for r in rows] == [('K', 0x8376AAF4, 1)], str(rows))
    # (M3) a negative constant index is signed
    rows.clear()
    qstores.collect(rows, calls, {3: ('a0', 0), 9: ('K', 0xFFFFFFF0)}, [0x1108, 'stwx', 'r4,r3,r9', [], False], SELF)
    check('stores twin (M3): li r9,-16 -> a0 -16 (not +4294967280)', [(r[4], r[5]) for r in rows] == [('a0', -16)], str(rows))
    # (H3) vector / atomic / cache stores are indexed with their widths
    rows.clear()
    qstores.collect(rows, calls, {3: ('a0', 64), 9: ('K', 16)}, [0x110C, 'stvx128', 'v63,r3,r9', [], False], SELF)
    qstores.collect(rows, calls, {3: ('a0', 64), 9: ('K', 16)}, [0x1110, 'stwcx', 'r4,r3,r9', [], False], SELF)
    qstores.collect(rows, calls, {3: ('a0', 64), 9: ('K', 0)}, [0x1114, 'dcbz', 'r3,r9', [], False], SELF)
    check('stores twin (H3): stvx128 w16 / stwcx w4 / dcbz w32 rows at a0 +80 / +80 / +64',
          [(r[2], r[3], r[5]) for r in rows] == [('stvx128', 16, 80), ('stwcx', 4, 80), ('dcbz', 32, 64)], str(rows))
    check('stores twin (H3): the tables carry the vector, atomic and cache forms', 'stvx128' in qstores.IDX and 'stvewx128' in qstores.IDX and 'stdcx' in qstores.IDX and 'dcbz128' in qstores.CACHE)
    # (H1) block writers: memcpy, a thunk to it (resolved at build), zero-fill (size in r4), memset, a tail call
    calls.clear()
    st2 = {3: ('a1', 16), 5: ('K', 92), 4: ('K', 24)}
    qstores.collect(rows, calls, st2, [0x100C, 'bl', '0x82C79000', [], False], SELF)
    check('stores twin: memcpy(r3 = a1+16, 92 bytes) is a block writer', calls == [(SELF, 0x100C, 'memcpy', 'a1', 16, 92)], str(calls))
    calls.clear()
    qstores.collect(rows, calls, st2, [0x1010, 'bl', '0x82C79004', [], False], SELF)
    check('stores twin: a call to another function is not a block writer', calls == [])
    saved = dict(qstores._CALLEES)
    qstores._CALLEES[0x82C2F7B8] = qstores.CALLEES[0x82C79000]          # the build adds 1-instruction `b` thunks to members
    qstores.collect(rows, calls, st2, [0x1014, 'bl', '0x82C2F7B8', [], False], SELF)
    check('stores twin (H1): the memcpy thunk 0x82C2F7B8 is a block writer', calls == [(SELF, 0x1014, 'memcpy', 'a1', 16, 92)], str(calls))
    qstores._CALLEES.clear()
    qstores._CALLEES.update(qstores.CALLEES)                              # planted: the thunk dropped -> the row must disappear
    calls.clear()
    qstores.collect(rows, calls, st2, [0x1014, 'bl', '0x82C2F7B8', [], False], SELF)
    check('stores twin (H1): planted twin, thunk not in the table -> no row', calls == [], str(calls))
    calls.clear()
    qstores.collect(rows, calls, st2, [0x1018, 'bl', '0x82CEBA60', [], False], SELF)
    check('stores twin (H1): zero-fill takes its size from r4 (24 B), not r5', calls == [(SELF, 0x1018, 'zerofill', 'a1', 16, 24)], str(calls))
    calls.clear()
    qstores.collect(rows, calls, st2, [0x101C, 'b', '0x82C79530', [], False], SELF)
    check('stores twin (H1): a tail call `b memset` is a block writer', calls == [(SELF, 0x101C, 'memset (tail)', 'a1', 16, 92)], str(calls))
    qstores._CALLEES.clear()
    qstores._CALLEES.update(saved)
    # (M6) add-formed base: add r11,r3,r4 (r4 = 434) then stb r25,6(r11)
    rows.clear()
    qstores._SH['func'] = None
    qstores.collect(rows, calls, {3: ('a0', 0), 4: ('K', 434)}, [0x1200, 'add', 'r11,r3,r4', [], False], 0x82000001)
    qstores.collect(rows, calls, {3: ('a0', 0), 4: ('K', 434)}, [0x1204, 'stb', 'r25,6(r11)', [], False], 0x82000001)
    check('stores twin (M6): add-formed base + displacement -> a0 +440, marked add-formed', rows == [(0x82000001, 0x1204, 'stb', 1, 'a0', 440, 0, 1)], str(rows))


def twin_switch_bias():
    lines = ['\t// addi r11,r24,-501', '\t// cmplwi cr6,r11,2', '\t// bgt cr6,0x82000100', '\t// bctr ', '\tswitch (ctx.r11.u32) {', '\tcase 0:', '\t\tgoto loc_82000010;',
             '\tcase 1:', '\t\tgoto loc_82000020;', '\tcase 2:', '\t\tgoto loc_82000020;', '\t}', 'loc_82000010:', '\t// blr']
    sws, _ = qgx.parse_switches(lines)
    check('switch twin (H2): addi r11,r24,-501 before the bound -> bias 501 on r24', sws and (sws[0]['bias'], sws[0]['src']) == (501, 'r24'), str(sws))
    lines[0] = '\t// mr r11,r24'
    sws, _ = qgx.parse_switches(lines)
    check('switch twin (H2): planted twin, no addi -> bias 0', sws and sws[0]['bias'] == 0, str(sws))
    lines[0], lines[1] = '\t// addi r11,r24,-501', '\t// cmplwi cr6,r3,2'                # a compare of another register is not the bound (L8)
    sws, _ = qgx.parse_switches(lines)
    check('switch twin (L8): a compare of another register gives no bound', sws and sws[0]['range'] is None, str(sws))


def twin_switch_body():
    lines = ['loc_82000010:', '\t// stw r28,40(r31)', '\t// bctrl ', '\t// stw r28,44(r31)', '\t// beq 0x82000020', '\t// blr']
    b = qgx.case_body(lines, {0x82000010: 0}, 1, set(), FakeCon(), {}, seed={28: 0})
    check('switch twin (M5): a seeded function-wide constant reaches the store; bctrl is an indirect call; one cond branch counted',
          b['stores'] == [('stw', 0, '40(r31)'), ('stw', 0, '44(r31)')] and b['calls'] and 'indirect' in b['calls'][0][0] and b['cond'] == 1, str(b))


def twin_includes():
    with tempfile.TemporaryDirectory() as d:
        os.makedirs(os.path.join(d, 'src'))
        for n, body in (('c.h', ''), ('b.h', '#include "c.h"\n'), ('a.h', '#include "b.h"\n'), ('u.cpp', '#include "a.h"\n'), ('z.cpp', '#include "c.h"\n'), ('n.cpp', '// #include "c.h"\n')):
            open(os.path.join(d, 'src', n), 'w').write(body)
        r = qgx.cmd_includes_graph(d, 'src/c.h', 'transitive')
        got = {k for _, k in r['rows']}
        check('includes twin: transitive includers of c.h = b.h a.h u.cpp z.cpp', got == {'src/b.h', 'src/a.h', 'src/u.cpp', 'src/z.cpp'}, str(got))
        check('includes twin: a commented #include is not an edge (n.cpp absent)', 'src/n.cpp' not in got)
        r = qgx.cmd_includes_graph(d, 'src/c.h', 'included-by')
        check('includes twin: --included-by = direct only (b.h, z.cpp)', {k for _, k in r['rows']} == {'src/b.h', 'src/z.cpp'})
        r = qgx.cmd_includes_graph(d, 'src/u.cpp', 'closure')
        check('includes twin: closure of u.cpp = a.h b.h c.h', {k for _, k in r['rows']} == {'src/a.h', 'src/b.h', 'src/c.h'})


# ------------------------------------------------------------------------------------------ real answers
def real_text():
    try:
        qgx._game_root()
        got = {282: qgx.cmd_text(282), 303: qgx.cmd_text(303), 60100: qgx.cmd_text(60100)}
    except (qgx.GxError, OSError, SystemExit) as e:
        return skip('text (real)', str(e))
    # docs/formats/string_pac.md (S101 lane 9): ids 282 / 303 (menu labels) and name 60100; asserted by hash + length, not by text
    check('text 282 = the 12-character label (sha1 a246c02ef30c)', got[282] and fp(got[282][0]['text']) == ('a246c02ef30c', 12))
    check('text 303 = the 10-character label (sha1 af1a4df9a7e2)', got[303] and fp(got[303][0]['text']) == ('af1a4df9a7e2', 10))
    check('text 60100 = an 8-character name (sha1 a375e81c680b), entry 5000', got[60100] and fp(got[60100][0]['text']) == ('a375e81c680b', 8) and got[60100][0]['entry'] == '5000')
    check('text planted twin: 282 is not the 303 text', got[282][0]['text'] != got[303][0]['text'])
    check('text: an id in no table gives no hit', qgx.cmd_text(1999999) == [])


def real_csdt():
    try:
        qgx._game_root()
        eras = [qgx.load_csdt(e)[0] for e in range(6)]
    except (qgx.GxError, OSError, SystemExit, IndexError) as e:
        return skip('csdt (real)', str(e))
    # docs/systems/wm30.md (S103 GTW): "counts 422 / 307 / 212 / 538 / 509" per era (= our records + the header record); keys pairwise disjoint; U005 = union of 0-4
    counts = [len(e) + 1 for e in eras[:5]]
    check('csdt: era record counts 422 / 307 / 212 / 538 / 509 (wm30.md, + header record)', counts == [422, 307, 212, 538, 509], str(counts))
    ks = [set(r['key'] for r in e) for e in eras[:5]]
    shared = {(i, j): sorted(ks[i] & ks[j]) for i in range(5) for j in range(i + 1, 5) if ks[i] & ks[j]}
    # S105 QG corrects wm30.md ("pairwise disjoint"): eras 0 and 1 share exactly two name captions, keys 60003 / 60005, with identical text
    check('csdt: only eras 0 / 1 share keys, exactly 60003 and 60005, same text', shared == {(0, 1): [60003, 60005]} and all([r['text'] for r in eras[0] if r['key'] == k] == [r['text'] for r in eras[1] if r['key'] == k] for k in (60003, 60005)), str(shared))
    check('csdt: U005 = exactly the union of eras 0-4 (360_HABITS row 18)', set(r['key'] for r in eras[5]) == set().union(*ks))
    r0 = eras[0][0]
    check('csdt: era 0 first caption = key 10, frames 256-804, 70-character text (sha1 4faf0121f752)', (r0['key'], r0['start'], r0['end']) == (10, 256, 804) and fp(r0['text']) == ('4faf0121f752', 70), str((r0['key'], r0['start'], r0['end'])))
    check('csdt: consecutive captions chain (key 10 ends where key 20 starts)', eras[0][1]['start'] == r0['end'])


def real_switch():
    try:
        import q
        con = q.connect(q.DB)
        r = qgx.cmd_switch(con, 0x828519C8)
    except Exception as e:     # noqa: BLE001 - no index / no generated source on this machine
        return skip('switch (real)', repr(e)[:80])
    sw = r['switches'][0]
    byc = {n: g for g in sw['groups'] for n in g['cases']}
    check('switch sub_828519C8: 14 cases 0..13, default 82851D10', sw['ncases'] == 14 and sw['default'] == 0x82851D10, str((sw['ncases'], sw['default'])))
    check('switch: case 0 -> @82851D14 with li r3=0x4F0', byc[0]['target'] == 0x82851D14 and (3, 0x4F0) in byc[0]['consts'])
    check('switch: cases 12 / 13 store 3 / 4 into 48(r31) (the li constants q decomp drops)', ('stw', 3, '48(r31)') in byc[12]['stores'] and ('stw', 4, '48(r31)') in byc[13]['stores'])
    check('switch: planted twin: case 12 does NOT store 4', ('stw', 4, '48(r31)') not in byc[12]['stores'])
    check('switch: cases 2 and 7 share one target', byc[2] is byc[7])
    check('switch (M5): the function-wide constant r28=0 reaches the stores of cases 3,4,5 / default', ('stw', 0, '40(r31)') in byc[3]['stores'], str(byc[3]['stores']))
    try:
        r2 = qgx.cmd_switch(con, 0x8264FAB0)
    except Exception as e:     # noqa: BLE001
        return skip('switch biased (real)', repr(e)[:80])
    s2 = r2['switches'][0]
    case501 = [g for g in s2['groups'] if 0 in g['cases']]
    check('switch (H2): sub_8264FAB0 is on r24 - 501 and case VALUE 501 (index 0) -> @82652598', s2['bias'] == 501 and s2['src'] == 'r24' and case501 and case501[0]['target'] == 0x82652598, str((s2['bias'], s2['src'])))


def real_stores():
    try:
        import build_fields as bf
        gen, image, _ = bf.default_paths(None, None)
        con = qstores.open_db(gen, image, quiet=True)
    except (SystemExit, OSError, Exception) as e:     # noqa: BLE001
        return skip('stores (real)', repr(e)[:80])
    idx, _ = qstores.query(con, 525481)
    got = {r[0] for r in idx if r[2] == 'stbx'}
    # `q imm 0x804A9` (T1): 7 functions build the constant 0x804A9; three of them use it as the index of an stbx (q field never modelled indexed stores)
    check('stores: +525481 (0x804A9) stbx writers = sub_8297A808 sub_8298A150 sub_82B17208', got == {0x8297A808, 0x8298A150, 0x82B17208}, str(sorted(hex(x) for x in got)))
    check('stores planted twin: no stbx writer at +2000000001', not qstores.query(con, 2000000001)[0])
    _, cpy = qstores.query(con, 0, 'a0')
    check('stores: memcpy dest rows exist (a0 +0)', len(cpy) > 0)
    # T1 (S105): the apply writer of G+15229 is `addi r3,r30,15148; li r5,92; bl 0x82c2f7b8` (the memcpy THUNK) at 0x822CA0E4
    _, cpy = qstores.query(con, 15229)
    check('stores (H1): apply writer 0x822CA0E4 (memcpy thunk, +15148, 92 B) covers +15229', any(r[1] == 0x822CA0E4 and r[4] == 15148 and r[5] == 92 for r in cpy), str([hex(r[1]) for r in cpy]))
    check('stores (H1): planted twin, +16000 is not covered by 0x822CA0E4', not any(r[1] == 0x822CA0E4 for r in qstores.query(con, 16000)[1]))
    n = con.execute("SELECT COUNT(*) FROM idx WHERE mn='stvx128'").fetchone()[0]
    check('stores (H3): stvx128 rows exist (thousands)', n > 1000, str(n))


def real_includes():
    root = os.path.normpath(os.path.join(HERE, '..', '..'))
    if not os.path.exists(os.path.join(root, 'src', 'replay.h')):
        return skip('includes (real)', 'no src/replay.h')
    r = qgx.cmd_includes_graph(root, 'src/replay.h', 'transitive')
    got = {k for _, k in r['rows']}
    check('includes: src/replay.h is reached from src/director.h and src/dlc.h', {'src/director.h', 'src/dlc.h'} <= got)
    d = qgx.cmd_includes_graph(root, 'src/replay.h', 'included-by')
    check('includes: --included-by is a subset of --transitive', {k for _, k in d['rows']} <= got)


def main():
    for f in (twin_text, twin_csdt, twin_switch, twin_switch_bias, twin_switch_body, twin_strip_flags, twin_stores, twin_includes, real_text, real_csdt, real_switch, real_stores, real_includes):
        f()
    print('FAIL: %s' % ', '.join(FAILS) if FAILS else 'all ok')
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
