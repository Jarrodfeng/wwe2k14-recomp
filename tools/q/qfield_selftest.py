r"""Self-test of the field index (S103 lane QF): a fixture (synthetic generated sources + image, nothing from the game) and the
real-data proofs of the findings the index was built for. Gate lane `qf_fields`.

    py -I tools/q/qfield_selftest.py [--root .] [--no-real]

Fixture checks (exact sets, so a wrong fold fails): the typed writers of +108, the planted twin (an UNTYPED function writing
+108, and a typed one writing +112: neither may appear in the typed +108 query), a join that disagrees (the base must not
survive it), the global fold ([0x83930050]+32) and its flow into a callee, and the mutation check: the same fixture with the
slot's offset changed 108 -> 112 must move that function from the +108 answer to the +112 answer.
Real-data proofs (skipped with --no-real or when generated/ is missing; the index is built on first use, ~10 s):
  1. writers of +108 on vtable 0x8201F294 include sub_825BA448 (the stage-page fade timer);
  2. readers of [0x83930050]+32 include sub_82C302A0 (the task runner) and sub_82C2FEB8 (the registration);
  3. the reader of device +592 (sub_82513A38 0x82513B18) is typed 'r:82513B08' = the return of sub_82BA7748, and the writers of
     +592 AND +600 as 8-byte pairs are the known candidate list (sub_827F70D0 among them)."""
import contextlib
import io
import json
import os
import shutil
import hashlib
import sqlite3
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import build_fields as bf   # noqa: E402
import qfield               # noqa: E402

VT = 0x82100000


def fn(addr, lines, labels=()):
    out = [f'DEFINE_REX_FUNC(sub_{addr:08X}) {{', '\tREX_FUNC_PROLOGUE();']
    for ln in lines:
        if ln.startswith('loc_'):
            out.append(f'{ln}')
            continue
        if ln.startswith('goto '):
            out[-1] = out[-1]
            out.append(f'\tif (cond) goto {ln[5:]};')
            continue
        out.append(f'\t// {ln}')
        out.append('\t/* body */')
    out.append('}')
    return '\n'.join(out) + '\n'


def make_fixture(d, slot_off=108):
    gen = os.path.join(d, 'generated', 'default')
    os.makedirs(gen, exist_ok=True)
    funcs = [
        # ctor of the type: stores the vtable constant at a0+0
        (0x82000010, ['lis r11,-32240', 'stw r11,0(r3)', 'blr ']),
        # slot 0 of the vtable: writes +slot_off (108 in the good fixture)
        (0x82000100, [f'stw r4,{slot_off}(r3)', 'blr ']),
        # planted twin 1: an UNTYPED function writing +108 (not a slot, not a ctor)
        (0x82000200, ['stw r4,108(r3)', 'blr ']),
        # slot 1 of the vtable: writes the adjacent wrong offset +112
        (0x82000300, ['stw r4,112(r3)', 'blr ']),
        # global fold: r11 = [0x83930050]; read +32; pass it to the callee
        (0x82000400, ['lis r11,-31853', 'lwz r11,80(r11)', 'lwz r10,32(r11)', 'mr r3,r11', 'bl 0x82000500', 'blr ']),
        # the callee reads +32 of its a0
        (0x82000500, ['lwz r10,32(r3)', 'blr ']),
        # a join that disagrees: r11 = a1 on one path, a0 on the other; +40 after the join has no known base
        (0x82000600, ['cmpwi cr6,r5,0', 'beq cr6,0x82000614', 'goto loc_82000614', 'mr r11,r4', 'b 0x82000618', 'goto loc_82000618',
                      'loc_82000614:', 'mr r11,r3', 'loc_82000618:', 'lwz r10,40(r11)', 'blr ']),
        # a join that agrees: r11 = a0 on both paths
        (0x82000700, ['cmpwi cr6,r5,0', 'beq cr6,0x82000714', 'goto loc_82000714', 'mr r11,r3', 'b 0x82000718', 'goto loc_82000718',
                      'loc_82000714:', 'mr r11,r3', 'loc_82000718:', 'lwz r10,44(r11)', 'blr ']),
    ]
    text = '#include "x.h"\n' + ''.join(fn(a, ls) for a, ls in funcs)
    with open(os.path.join(gen, 'wwe2k14_recomp.0.cpp'), 'w', encoding='utf-8') as f:
        f.write(text)
    img = bytearray(0x100100)
    # vtable at VT: slot 0 = sub_82000100, slot 1 = sub_82000300, then a zero word (end)
    struct.pack_into('>II', img, VT - bf.IMAGE_BASE, 0x82000100, 0x82000300)
    # a word at each function start so "is a function" is by the generated list, not the image
    ip = os.path.join(d, 'image.bin')
    with open(ip, 'wb') as f:
        f.write(img)
    return gen, ip


def run(argv):
    buf = io.StringIO()
    err = io.StringIO()
    with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(err):
        rc = qfield.main(argv + ['--json'])
    if rc != 0:
        raise AssertionError(f'q field {argv} -> rc {rc}: {buf.getvalue()} {err.getvalue()}')
    return json.loads(buf.getvalue())


def funcs_of(res):
    return sorted({r['func'] for r in res['rows']})


def check(cond, msg, fails):
    if not cond:
        fails.append(msg)
        print('FAIL', msg)
    else:
        print('ok  ', msg)


def fixture_checks(fails):
    d = tempfile.mkdtemp(prefix='qf_fixture_')
    try:
        gen, img = make_fixture(d)
        db = os.path.join(d, 'f.db')
        base = ['--gen', gen, '--image', img, '--db', db]
        res = run(['108', '--vtable', hex(VT), '--writes'] + base)
        check(funcs_of(res) == ['sub_82000100'], f'typed writers of +108 = [sub_82000100] (got {funcs_of(res)})', fails)
        res = run(['108', '--writes'] + base + ['--no-rebuild'])
        check(funcs_of(res) == ['sub_82000100', 'sub_82000200'], 'untyped query sees both +108 writers incl. the planted twin', fails)
        res = run(['112', '--vtable', hex(VT), '--writes'] + base + ['--no-rebuild'])
        check(funcs_of(res) == ['sub_82000300'], 'typed writers of +112 = the adjacent slot only', fails)
        res = run(['32', '--global', '0x83930050'] + base + ['--no-rebuild'])
        check(funcs_of(res) == ['sub_82000400', 'sub_82000500'], f'[0x83930050]+32 readers incl. the callee via callers (got {funcs_of(res)})', fails)
        res = run(['32', '--global', '0x83930050', '--strict'] + base + ['--no-rebuild'])
        check(funcs_of(res) == ['sub_82000400'], '--strict drops the inferred-from-callers reader', fails)
        res = run(['40', '--reads'] + base + ['--no-rebuild'])
        check([r['root'] for r in res['rows']] == ['u'], 'a disagreeing join gives no base (root u)', fails)
        res = run(['44', '--reads'] + base + ['--no-rebuild'])
        check([r['root'] for r in res['rows']] == ['a0'], 'an agreeing join keeps the base (a0)', fails)
        t = run(['type', hex(VT)] + base + ['--no-rebuild'])
        offs = {e['off']: e for e in t['layout']}
        check(sorted(offs) == [0, 108, 112] and offs[0]['e_writer'] == 'sub_82000010' and offs[108]['e_writer'] == 'sub_82000100'
              and offs[112]['e_writer'] == 'sub_82000300', 'type layout: +0 (the constructor), +108 and +112, one writer each', fails)
        # the planted wrong offset: the slot writes +112 instead of +108
        gen2, img2 = make_fixture(os.path.join(d, 'm'), slot_off=112)
        base2 = ['--gen', gen2, '--image', img2, '--db', os.path.join(d, 'm.db')]
        res = run(['108', '--vtable', hex(VT), '--writes'] + base2)
        check(funcs_of(res) == [], 'mutation: slot moved to +112 -> the +108 answer is empty (the wrong offset is caught)', fails)
        res = run(['112', '--vtable', hex(VT), '--writes'] + base2 + ['--no-rebuild'])
        check(funcs_of(res) == ['sub_82000100', 'sub_82000300'], 'mutation: both slots now answer +112', fails)
    finally:
        shutil.rmtree(d, ignore_errors=True)


def sha(path):
    with open(path, 'rb') as f:
        return hashlib.sha256(f.read()).hexdigest()


def rows_of(db):
    c = sqlite3.connect(f'file:{db}?mode=ro', uri=True)
    try:
        return c.execute('select count(*) from rows').fetchone()[0], c.execute("select v from meta where k='signature'").fetchone()[0]
    finally:
        c.close()


def spawn(argv, env_extra):
    env = dict(os.environ)
    env.update(env_extra)
    return subprocess.Popen([sys.executable, '-I', os.path.join(HERE, 'qfield.py')] + argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, env=env)


def schema_name_checks(fails):
    """S105 D (F1): the schema is in the file name; a q of another schema never opens, wipes or rebuilds another schema's file (modelled on qasset_selftest)."""
    if not hasattr(bf, 'db_name'):
        check(False, 'build_fields.db_name() exists (the schema is in the file name)', fails)
        return
    d = tempfile.mkdtemp(prefix='qf_schema_')
    try:
        gen, img = make_fixture(d)
        idx = os.path.join(d, 'out', 'harness', 'index')
        os.makedirs(idx, exist_ok=True)
        legacy = os.path.join(idx, 'fields.db')           # the pre-S105 name (schema in the code only)
        with open(legacy, 'wb') as f:
            f.write(b'LEGACY FIELD INDEX OF ANOTHER SCHEMA\0' * 50)
        h0 = sha(legacy)
        base = ['--gen', gen, '--image', img]
        res = run(['108', '--vtable', hex(VT), '--writes'] + base)
        mine = os.path.join(idx, bf.db_name())
        check(bf.db_name() == 'fields.s%d.db' % bf.SCHEMA and os.path.exists(mine), f'default path: built as {bf.db_name()}', fails)
        check(sha(legacy) == h0, 'the old-name fields.db was left byte-identical by a schema-%d q call' % bf.SCHEMA, fails)
        check(funcs_of(res) == ['sub_82000100'], 'the answer is unchanged (typed writers of +108)', fails)
        check(any('fields.db' in x for x in bf.other_index_files(mine)), 'other_index_files names the older file', fails)
        info = run(['info'] + base + ['--no-rebuild'])
        check('fields.db' in ' '.join(info.get('other_index_files', [])), 'q field info names the other-schema file (left alone)', fails)
        h1 = sha(mine)
        # a schema bump (N-1 -> N): the schema-2 file stays byte-identical, the schema-3 file is built beside it
        old_schema = bf.SCHEMA
        bf.SCHEMA = old_schema + 1
        try:
            res = run(['108', '--vtable', hex(VT), '--writes'] + base)
            new = os.path.join(idx, bf.db_name())
            check(new != mine and os.path.exists(new) and sha(mine) == h1 and sha(legacy) == h0,
                  f'a schema-{bf.SCHEMA} call built {bf.db_name()} beside it; the schema-{old_schema} file and the legacy file are byte-identical', fails)
            check(funcs_of(res) == ['sub_82000100'], 'the answer is the same from the new file', fails)
        finally:
            bf.SCHEMA = old_schema
    finally:
        shutil.rmtree(d, ignore_errors=True)


def dead_pid():
    """the pid of a process that has finished"""
    r = subprocess.run([sys.executable, '-c', 'import os; print(os.getpid())'], capture_output=True, text=True)
    return int(r.stdout.strip())


def lock_checks(fails):
    """S105 D (F2 + review F-1/F-2/F-5): two rebuilds, the first one's "building" line awaited, then the second started: exactly one builds (per-process temp, rebuild lock), the other answers
    from the old file and says so; the survivor has no doubled rows. A dead holder's lock is taken over at once (pid printed), a live idle one after the timeout, a live fresh one is waited
    for but never past WWE_QFIELD_WAIT; a waiter finds the build done; a reader holding the file across the swap does not crash the builder."""
    d = tempfile.mkdtemp(prefix='qf_lock_')
    try:
        gen, img = make_fixture(d)
        db = os.path.join(d, 'f.db')
        base = ['--gen', gen, '--image', img, '--db', db]
        run(['108', '--writes'] + base)                      # the "old" index (signature A)
        n_single, sig_old = rows_of(db)
        src = os.path.join(gen, 'wwe2k14_recomp.0.cpp')
        with open(src, 'a', encoding='utf-8') as f:
            f.write('// the codegen changed\n')               # signature B: every caller now wants a rebuild
        ref = os.path.join(d, 'ref.db')                      # the reference: ONE build of the changed fixture into another file
        run(['108', '--writes', '--gen', gen, '--image', img, '--db', ref])
        n_ref, sig_new = rows_of(ref)
        argv = ['108', '--writes', '--json'] + base
        env = {'WWE_QFIELD_BUILD_HOLD': '8'}                 # test hook: the lock holder sleeps while it holds the lock
        p1 = spawn(argv, env)
        line = ''
        while 'q field: building' not in line:               # start the second caller only once the first HOLDS the lock (no race on a loaded PC)
            line = p1.stderr.readline()
            if not line:
                break
        p2 = spawn(argv, env)
        outs = [p.communicate(timeout=300) for p in (p1, p2)]
        outs[0] = (outs[0][0], line + outs[0][1])
        rcs = [p1.returncode, p2.returncode]
        building = sum('q field: building the field index' in e for _, e in outs)
        old = sum('answering from the old index' in e for _, e in outs)
        check(rcs == [0, 0], f'both callers answer (rc {rcs})', fails)
        check(building == 1 and old == 1, f'exactly one built, the other answered from the old index and said so (built {building}, old {old})', fails)
        try:
            ans = [sorted({r['func'] for r in json.loads(o)['rows']}) for o, _ in outs]
        except ValueError:
            ans = None
        check(ans is not None and ans[0] == ans[1] == ['sub_82000100', 'sub_82000200'], f'both answers are the +108 writers (got {ans})', fails)
        n_after, sig_after = rows_of(db)
        check(n_after == n_ref == n_single and sig_after == sig_new, f"the survivor has one build's rows ({n_after} == {n_ref}), no doubles, the new signature", fails)
        left = [x for x in os.listdir(d) if x.endswith('.tmp') or x.endswith('.lock')]
        check(not left, f'no temp or lock file left behind (found {left})', fails)

        lk = db + '.lock'
        # F-1: a DEAD holder's lock is taken over at once, however fresh it is (a killed q never runs its `finally`)
        with open(src, 'a', encoding='utf-8') as f:
            f.write('// again\n')
        dp = dead_pid()
        with open(lk, 'w') as f:
            f.write(str(dp))
        t0 = time.time()
        p = spawn(['108', '--writes', '--json'] + base, {})
        o, e = p.communicate(timeout=300)
        check(p.returncode == 0 and 'taking over a stale rebuild lock' in e and str(dp) in e and 'q field: building the field index' in e and not os.path.exists(lk) and time.time() - t0 < 60,
              'F-1 a fresh lock of a DEAD pid is taken over at once (pid printed), the index rebuilt, the lock released', fails)
        # a live holder (this process) with an idle lock: the age backstop takes it over
        with open(src, 'a', encoding='utf-8') as f:
            f.write('// again 2\n')
        with open(lk, 'w') as f:
            f.write(str(os.getpid()))
        t = time.time() - 3600
        os.utime(lk, (t, t))
        p = spawn(['108', '--writes', '--json'] + base, {'WWE_QFIELD_LOCK_STALE': '600'})
        o, e = p.communicate(timeout=300)
        check(p.returncode == 0 and 'taking over a stale rebuild lock' in e and not os.path.exists(lk), 'a LIVE holder idle past the timeout is taken over by the age backstop', fails)
        # the heartbeat: touch_lock makes an old live lock fresh
        with open(lk, 'w') as f:
            f.write(str(os.getpid()))
        os.utime(lk, (t, t))
        stale_before = bf.lock_is_stale(lk)
        bf.touch_lock(lk)
        check(stale_before and not bf.lock_is_stale(lk), 'the heartbeat (touch_lock) turns an idle live lock into a fresh one', fails)
        # release_lock removes only OUR lock
        with open(lk, 'w') as f:
            f.write(str(dp))
        bf.release_lock(lk)
        check(os.path.exists(lk), 'release_lock leaves another pid\'s lock alone', fails)
        os.remove(lk)

        # F-1: no old index, a LIVE fresh holder: wait, but never past WWE_QFIELD_WAIT (an agent's tool timeout is shorter than the 20 min backstop)
        os.remove(db)
        with open(lk, 'w') as f:
            f.write(str(os.getpid()))
        t0 = time.time()
        p = spawn(['108', '--writes', '--json'] + base, {'WWE_QFIELD_WAIT': '2'})
        o, e = p.communicate(timeout=300)
        check(p.returncode == 2 and 'is still building the field index' in (o + e) and str(os.getpid()) in (o + e) and time.time() - t0 < 60 and not os.path.exists(db),
              'F-1 a live fresh lock and no index: it waits at most WWE_QFIELD_WAIT s, then fails with "pid N is still building, try again"', fails)
        # the waiter finds the build done (if_stale path: no second build)
        p = spawn(['108', '--writes', '--json'] + base, {'WWE_QFIELD_WAIT': '60'})
        line = ''
        while 'waiting for it' not in line:
            line = p.stderr.readline()
            if not line:
                break
        done = os.path.join(d, 'done.db')
        run(['108', '--writes', '--gen', gen, '--image', img, '--db', done])
        os.replace(done, db)
        os.remove(lk)
        o, e = p.communicate(timeout=300)
        check(p.returncode == 0 and 'q field: building the field index' not in e and rows_of(db)[0] == n_ref, 'a waiter whose holder finished answers from the finished index and builds nothing', fails)

        # F-2: a reader holding the index open across the swap (Windows refuses the replace): no traceback, the old file answers, no temp or lock left
        with open(src, 'a', encoding='utf-8') as f:
            f.write('// again 3\n')
        h_before = sha(db)
        rd = sqlite3.connect(f'file:{db}?mode=ro', uri=True)
        rd.execute('select count(*) from rows').fetchone()
        try:
            p = spawn(['108', '--writes', '--json'] + base, {'WWE_QFIELD_SWAP_WAIT': '1'})
            o, e = p.communicate(timeout=300)
        finally:
            rd.close()
        left = [x for x in os.listdir(d) if '.tmp' in x or x.endswith('.lock')]
        check(p.returncode == 0 and 'Traceback' not in e and 'a reader holds the index' in e and sha(db) == h_before and not left,
              f'F-2 a reader holding the file across the swap: no crash, "a reader holds the index" said, the old file kept, no temp / lock left (rc {p.returncode}, left {left})', fails)
    finally:
        shutil.rmtree(d, ignore_errors=True)


def real_checks(root, fails):
    tmpd = tempfile.mkdtemp(prefix='qf_real_')
    saved = os.environ.get('WWE_QFIELD_DB')
    os.environ['WWE_QFIELD_DB'] = os.path.join(tmpd, 'fields.real.db')      # S105 D (F-3): never the shared out/harness/index of any checkout
    try:
        _real_checks(root, fails)
    finally:
        if saved is None:
            os.environ.pop('WWE_QFIELD_DB', None)
        else:
            os.environ['WWE_QFIELD_DB'] = saved
        shutil.rmtree(tmpd, ignore_errors=True)


def _real_checks(root, fails):
    base = ['--root', root]
    gen, image, _db = bf.default_paths(root)
    if not os.path.exists(image):
        print('skip real proofs: no image.bin (out/harness/index)')
        return
    res = run(['108', '--vtable', '0x8201F294', '--writes'] + base)
    check('sub_825BA448' in funcs_of(res), 'proof 1: writers of +108 on vtable 0x8201F294 include sub_825BA448', fails)
    res = run(['32', '--global', '0x83930050'] + base)
    got = funcs_of(res)
    check('sub_82C302A0' in got and 'sub_82C2FEB8' in got, 'proof 2: readers of [0x83930050]+32 include sub_82C302A0 (runner) and sub_82C2FEB8', fails)
    res = run(['592', '--func', 'sub_82513A38'] + base)
    check(any(r['root'] == 'r:82513B08' and r['rw'] == 'R' and r['width'] == 8 for r in res['rows']),
          'proof 3a: the LightMapParam reader (0x82513B18) is +592 of the return of the call at 0x82513B08 (sub_82BA7748)', fails)
    both = set()
    r6 = run(['592', '--writes', '--width', '8', '--limit', '5000'] + base)
    r7 = run(['600', '--writes', '--width', '8', '--limit', '5000'] + base)
    both = set(funcs_of(r6)) & set(funcs_of(r7))
    check('sub_827F70D0' in both, f'proof 3b: the 8-byte pair writers of +592/+600 ({len(both)} functions) include sub_827F70D0', fails)


def main():
    root = '.'
    args = sys.argv[1:]
    if '--root' in args:
        root = args[args.index('--root') + 1]
    fails = []
    fixture_checks(fails)
    schema_name_checks(fails)   # S105 D
    lock_checks(fails)          # S105 D
    if '--no-real' not in args:
        real_checks(root, fails)
    print('PASS' if not fails else f'FAIL ({len(fails)})')
    return 1 if fails else 0


if __name__ == '__main__':
    import multiprocessing
    multiprocessing.freeze_support()
    sys.exit(main())
