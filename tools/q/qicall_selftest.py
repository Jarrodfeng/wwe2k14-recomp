r"""qicall_selftest: pins the S110 ICALL results (docs\research\s110\INDIRECT.md): `q callers` shows the callers that reach a function through an indirect call and says what its list
cannot see. 0 game runs. Needs the code index (with its `tables`), image.bin and generated\ (else the real-data checks SKIP; the unit checks always run).

  unit   CTR set before a conditional branch survives it (qx7.py: `st.ctr = None` was dropped); `lwzx rD,rA,rB` is tracked with either operand the constant base;
         (S110 review) bdnz clears CTR; `bnectr` is a tail call; V_this falls back to the offset set for a function called directly or listed by a 'run' table;
         a vtable short of the slot makes the set unknown; the `incomplete:` count takes F sites at the slot offset.
  q      `q callers 0x833693C8` (BLD7): 0 direct callers, and the 16 indirect sites in 7 functions through slot +4 of the object embedded at +1184 (0x8233C8F4 in sub_8233C7D8 ...);
         `q callers 0x82BDB460` (the packer's virtual link): its direct caller sub_82C18668 AND the line `incomplete: 332 sites call slot +56 ...`.
  count  the 1,159 switch `bctr` are not calls (meta switch_bctr; no icall row of kind switch); 36,532 indirect calls = 35,001 bctrl + 2,690 bctr - 1,159;
         the shape and status counts are exactly the pinned ones (EXACT_SHAPES / EXACT_STATUS, S110 CLASSFLOW review).
Run:  py -I tools\q\qicall_selftest.py [--q PATH_TO_q.py]      (--q = another q.py, e.g. the old one: the `q` checks must then FAIL; they print which)
Env:  WWE_Q_INDEX / WWE_QVCALL_DB = an index and an icall table beside the shared ones (a builder's scratch); WWE_Q_NO_AUTOUPDATE=1 is set for the calls.
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qx7  # noqa: E402

EXACT_FROM = 'commit 492295047, S110 CLASSFLOW review'
EXACT_SHAPES = {'A': 148, 'F': 4655, 'G': 57, 'K': 170, 'R': 6, 'T': 18, 'T_open': 246, 'U': 367, 'V_const': 22, 'V_exact': 2, 'V_open': 23716, 'V_this': 7126}
EXACT_STATUS = {'cand': 3360, 'exact': 4651, 'narrow': 757, 'offset': 26911, 'open': 854}
BLD7_SITES = {0x8233C8F4: 0x8233C7D8, 0x824EDC1C: 0x824EDB50, 0x824EDCF0: 0x824EDB50, 0x824EDF94: 0x824EDDB0, 0x824EDFF0: 0x824EDDB0, 0x824EE04C: 0x824EDDB0, 0x824EE0A8: 0x824EDDB0,
              0x827646D0: 0x82764608, 0x827647A4: 0x82764608, 0x82765204: 0x82764FF0, 0x82765260: 0x82764FF0, 0x827652BC: 0x82764FF0, 0x82765318: 0x82764FF0,
              0x829927FC: 0x829927B8, 0x8341BFD4: 0x8341BF58, 0x8341C430: 0x8341BF58}


class Fail(Exception):
    pass


def check(cond, what, out=''):
    if not cond:
        raise Fail(f'{what}\n--- output ---\n{str(out)[-1200:]}')
    print(f'PASS  {what}')


def unit_checks():
    ins = [[0x82001000, 'lwz', 'r11,0(r3)', None], [0x82001004, 'lwz', 'r10,4(r11)', None], [0x82001008, 'mtctr', 'r10', None], [0x8200100C, 'beq', 'cr6,0x82001014', None],
           [0x82001010, 'li', 'r4,1', None], [0x82001014, 'bctrl', '', None], [0x82001018, 'blr', '', None]]
    hits = []
    qx7.run_function(ins, lambda a: False, frozenset(), lambda k, a, info, tr: hits.append((k, a)) if k == 'vcall' else None)
    check(hits == [('vcall', 0x82001014)], 'unit: CTR set before a conditional branch is still known at the bctrl after the label (it was lost at the join)', hits)
    ins = [[0x82002000, 'lis', 'r11,-32000', None], [0x82002004, 'addi', 'r11,r11,16', None], [0x82002008, 'lwzx', 'r10,r11,r9', None], [0x8200200C, 'mtctr', 'r10', None],
           [0x82002010, 'bctrl', '', None]]
    seen = []
    qx7.run_function(ins, lambda a: False, frozenset(), lambda k, a, info, tr: seen.append(info[0]) if k == 'vcall' else None)
    check(len(seen) == 1 and seen[0][0] == 'lwzx' and seen[0][1][0] == 'const', 'unit: `lwzx rD,rA,rB` gives a lwzx def whose constant base is kept (the table base)', seen)
    # S110 CLASSFLOW review: shape T (a table of function words) only for an index scaled by 4; sub_827DCDB0 0x827DCEC4 reads word 0 of 16-byte records
    # (`rlwinm r10,r11,4,0,27; lwzx r10,r10,r28`): the word run from the base gave 1 "exact" target, coverage run 083 saw 4 others
    for sh, me, want in ((4, 27, 'T_open'), (2, 29, 'T')):
        ins = [[0x82002100, 'lis', 'r11,-32000', None], [0x82002104, 'addi', 'r11,r11,16', None], [0x82002108, 'rlwinm', f'r9,r4,{sh},0,{me}', None],
               [0x8200210C, 'lwzx', 'r10,r11,r9', None], [0x82002110, 'mtctr', 'r10', None], [0x82002114, 'bctrl', '', None]]
        got = []
        qx7.run_function(ins, lambda a: False, frozenset(), lambda k, a, info, tr: got.append(qx7.classify(info[0], tr, info[2])[0]) if k == 'vcall' else None)
        check(got == [want], f'unit: lwzx on a constant base with the index shifted by {sh} is shape {want}', got)
    # S110 review: CTR kept across a branch is only right while every CTR write clears it; bdnz decrements CTR
    ins = [[0x82003000, 'lwz', 'r11,0(r3)', None], [0x82003004, 'lwz', 'r10,4(r11)', None], [0x82003008, 'mtctr', 'r10', None], [0x8200300C, 'addi', 'r9,r9,1', None],
           [0x82003010, 'bdnz', '0x8200300c', None], [0x82003014, 'bctrl', '', None], [0x82003018, 'blr', '', None]]
    hits = []
    qx7.run_function(ins, lambda a: False, frozenset(), lambda k, a, info, tr: hits.append((k, a)) if k == 'vcall' else None)
    check(hits == [], 'unit: a bdnz between mtctr and bctrl leaves CTR unknown (no vcall def at the bctrl)', hits)
    ins = [[0x82004000, 'lis', 'r4,-31843', None], [0x82004004, 'lwz', 'r0,-16448(r4)', None], [0x82004008, 'mtctr', 'r0', None], [0x8200400C, 'bnectr', '', None],
           [0x82004010, 'blr', '', None]]
    hits = []
    qx7.run_function(ins, lambda a: False, frozenset(), lambda k, a, info, tr: hits.append((k, a)) if k == 'vtail' else None)
    check(hits == [('vtail', 0x8200400C)], 'unit: a conditional `bnectr` is an indirect tail call (sub at 0x8200400C seen)', hits)
    # S110 review: V_exact takes the vtable stored at +0 of the object on every path to the call, not the function's last store
    VT = 0x82010000
    head = [[0x82005000, 'mr', 'r31,r3', None], [0x82005004, 'lis', 'r30,-32255', None], [0x82005008, 'addi', 'r30,r30,0', None]]
    call = [[0x82005100, 'lwz', 'r11,0(r31)', None], [0x82005104, 'lwz', 'r10,4(r11)', None], [0x82005108, 'mtctr', 'r10', None], [0x8200510C, 'bctrl', '', None]]
    store = [[0x82005200, 'stw', 'r30,0(r31)', None]]      # r30: a call keeps it
    branch = [[0x82005010, 'beq', 'cr6,0x82005100', None]]
    tail = [[0x82005300, 'blr', '', None]]
    for name, body, want in (('store before the call', head + store + call + tail, True), ('store after the call', head + call + store + tail, False),
                             ('store on one path only', head + branch + store + call + tail, False)):
        got = []
        qx7.run_function(body, lambda a: a == VT, frozenset(), lambda k, a, info, tr: got.append(qx7.classify(info[0], tr, info[2])[0]) if k == 'vcall' else None)
        check(got == (['V_exact'] if want else ['V_this']), f'unit: V_exact from the vtable store that reaches the call ({name})', got)
    resolver_checks()


class _Img:
    def __init__(self, words):
        self.w = words

    def word(self, a):
        return self.w.get(a)


def resolver_checks():
    """S110 review: V_this names the slot of the tables that list the function only when nothing calls it directly (a super call passes a derived `this`) and no
    table that lists it is a 'run' table; a vtable that does not reach the slot makes the set unknown, never a smaller set"""
    import sqlite3
    FA, FB, FC, FD, FE = 0x82100000, 0x82100100, 0x82100200, 0x82100300, 0x82100400
    T1, T2 = 0x82010000, 0x82010100
    words = {T1: FA, T1 + 4: FB, T2: FC, T2 + 4: FD}

    def icon(edges=(), rule='code', t2len=2):
        c = sqlite3.connect(':memory:')
        c.executescript('''CREATE TABLE funcs(addr INTEGER); CREATE TABLE tables(start INTEGER, rule TEXT, len INTEGER); CREATE TABLE edges(src INTEGER, dst INTEGER, kind TEXT);
            CREATE TABLE rtti_vtable(vt INTEGER, td INTEGER); CREATE TABLE rtti_class(td INTEGER, name TEXT); CREATE TABLE rtti_base(cls TEXT, base TEXT);''')
        c.executemany('INSERT INTO funcs VALUES (?)', [(x,) for x in (FA, FB, FC, FD, FE)])
        c.executemany('INSERT INTO tables VALUES (?,?,?)', [(T1, rule, 2), (T2, 'code', t2len)])
        c.executemany('INSERT INTO edges VALUES (?,?,?)', list(edges))
        return c

    site = (0x82100010, FA, 'call', 'V_this', 4, 0, ('arg', 'r3'))
    t, how = qx7.Resolver(icon(), _Img(words), set()).resolve(site)
    check(t == {FB} and how == 'this-class', 'unit: V_this of a function only its own table lists and nothing calls directly: the slot of that table', (t, how))
    t, how = qx7.Resolver(icon(edges=[(FE, FA, 'call')]), _Img(words), set()).resolve(site)
    check(how == 'offset', 'unit: V_this of a function called directly (a super call can pass another class): the offset set, not one target', (t, how))
    t, how = qx7.Resolver(icon(rule='run'), _Img(words), set()).resolve(site)
    check(how == 'offset', "unit: V_this of a function listed by a 'run' table (no code loads it: no object's vtable start): the offset set", (t, how))
    R = qx7.Resolver(icon(t2len=1), _Img(words), set())
    check(R.slot_targets({T1, T2}, 4) is None, 'unit: slot_targets of a vtable set where one table ends before the slot is unknown (None), not the other tables alone',
          R.slot_targets({T1, T2}, 4))
    # the incomplete count of a slot function takes the F sites at its offset too (0x8336CA1C `lwz r11,36(r31); lwz r10,8(r11); bctrl` is shape F)
    import tempfile
    d = tempfile.mkdtemp(prefix='qicall_')
    p = os.path.join(d, 'vcall.fixture.db')
    v = sqlite3.connect(p)
    v.executescript('''CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT); CREATE TABLE icall(site INTEGER, func INTEGER, kind TEXT, shape TEXT, off INTEGER, recv TEXT, status TEXT, n INTEGER);
        CREATE TABLE icall_target(site INTEGER, target INTEGER, how TEXT); CREATE TABLE fstore(kind TEXT, key INTEGER, value INTEGER);''')
    v.execute('INSERT INTO meta VALUES (?, ?)', ('version', str(qx7.VERSION)))
    v.executemany('INSERT INTO icall VALUES (?,?,?,?,?,?,?,?)', [(0x82200000, FE, 'call', 'V_open', 4, '', 'offset', 2), (0x82200010, FE, 'call', 'F', 4, 'ptr:36:in', 'offset', 2),
                                                                  (0x82200020, FE, 'call', 'F', 8, 'ptr:36:in', 'offset', 2)])
    v.commit()
    v.close()
    c = sqlite3.connect(':memory:')
    c.executescript('CREATE TABLE ptrs(at INTEGER, value INTEGER, kind TEXT); CREATE TABLE tables(start INTEGER, rule TEXT, len INTEGER); CREATE TABLE imms(value INTEGER, op TEXT);')
    c.execute('INSERT INTO ptrs VALUES (?, ?, ?)', (T1 + 4, FB, 'func'))
    c.execute('INSERT INTO tables VALUES (?, ?, ?)', (T1, 'code', 2))
    r = qx7.icall_callers(c, FB, p)
    check(r['offsets'] == {4: 2} and r['n_incomplete'] == 2, 'unit: `incomplete:` of a slot +4 function counts the V and the F site at +4 (not the F at +8)', r)
    try:
        os.remove(p)
        os.rmdir(d)
    except OSError:
        pass


def run_q(q, *args):
    env = dict(os.environ, WWE_Q_NO_AUTOUPDATE='1')
    cmd = [sys.executable, q] + list(args)
    if os.environ.get('WWE_Q_INDEX'):
        cmd += ['--index', os.environ['WWE_Q_INDEX']]
    p = subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=240)
    return p.stdout + p.stderr


def q_checks(q):
    out = run_q(q, 'callers', '0x833693C8')
    got = {int(x, 16) for x in re.findall(r'(0x[0-9A-F]{8}) slot \+4', out)}
    check(out.startswith('sub_833693C8: 0 call sites'), 'q callers 0x833693C8: no direct caller', out)
    check('candidate indirect callers (table matched by field offset only; class not checked; 16 sites in 7 functions)' in out and '  indirect callers (' not in out
          and '0x8233C8F4 slot +4' in out and 'sub_8233C7D8' in out,
          'q callers 0x833693C8: the 16 sites in 7 functions through slot +4 (0x8233C8F4 in sub_8233C7D8) print under the candidate heading only', out)
    check(set(BLD7_SITES) <= got, 'q callers 0x833693C8: every one of the 16 sites of the note is named', out)


def q_checks_packer(q):
    out = run_q(q, 'callers', '0x82BDB460')
    check('sub_82C18668' in out and '0x82C186C8' in out, 'q callers 0x82BDB460: the direct caller is printed', out)
    # 332 V sites (the note) + 21 F sites at +56 (S110 review: a slot call on an embedded object reads its vtable in one load and is shape F)
    # S110 CLASSFLOW: 25 of the 353 are now exact / narrow (to other functions: none to the packer), so the line no longer counts them (327 before the review
    # made field / stack-slot results candidates)
    check('incomplete: 328 sites call slot +56' in out and 'with CTR unknown' in out, 'q callers 0x82BDB460: `incomplete: 328 sites call slot +56 (class not shown); M with CTR unknown`', out)


def count_checks():
    v = qx7.open_vdb()
    meta = dict(v.execute('SELECT key, value FROM meta').fetchall())
    check(meta.get('switch_bctr') == '1159', 'count: 1,159 switch bctr found and kept out of the calls', meta)
    check(v.execute("SELECT COUNT(*) FROM icall WHERE kind = 'switch' OR shape = 'S'").fetchone()[0] == 0, 'count: no switch row among the indirect calls')
    n = v.execute('SELECT COUNT(*) FROM icall').fetchone()[0]
    kinds = dict(v.execute('SELECT kind, COUNT(*) FROM icall GROUP BY kind').fetchall())
    check(n == 36533 and kinds == {'call': 35001, 'tail': 1532}, 'count: 36,533 indirect calls = 35,001 bctrl + (2,690 bctr - 1,159 switches) + 1 bnectr', (n, kinds))
    # S110 CLASSFLOW review: EXACT counts (the 1 %-of-all-sites tolerance let U/R 373 pass against the note's 611). They are what this code produces on
    # generated\ fingerprint f5ed94e0e44c727dd3206ae06a1c23f4 (commit: see EXACT_FROM); a new codegen or a rule change moves them: re-pin from the INFO line.
    by = dict(v.execute('SELECT shape, COUNT(*) FROM icall GROUP BY shape').fetchall())
    check(by == EXACT_SHAPES, f'count: shapes exactly as pinned ({EXACT_FROM})', by)
    t = dict(v.execute('SELECT status, COUNT(*) FROM icall GROUP BY status').fetchall())
    print(f'INFO  status: {t}')
    check(t == EXACT_STATUS, f'count: statuses exactly as pinned ({EXACT_FROM})', t)


def main():
    q = os.path.join(HERE, 'q.py')
    if '--q' in sys.argv:
        q = sys.argv[sys.argv.index('--q') + 1]
    fails = 0
    for step in (unit_checks, lambda: q_checks(q), lambda: q_checks_packer(q), count_checks):
        if step is not unit_checks and not os.path.isdir(qx7.GEN):
            print('SKIP  real-data checks (generated\\ is absent)')
            break
        try:
            step()
        except Fail as e:
            fails += 1
            print(f'FAIL  {e}')
    print('OK' if not fails else f'{fails} step(s) FAILED')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
