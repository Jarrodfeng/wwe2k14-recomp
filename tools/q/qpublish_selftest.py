r"""S110 QREBUILD selftest: a rebuilt index is published while readers hold the old file, a stale lookup answers at once, the linker map is not an input of
index.db, and the library index (fact.db) is not rebuilt by every call while a reader holds it. 0 game runs; everything runs in TEMP folders (a fixture
generated folder of two functions, a fixture linker map); the shared out\harness\index is never touched.

  A  two reader PROCESSES hold index.db open while a third process (the real build_index.py) rebuilds it: the build exits 0 without waiting, the new index waits
     beside the held file, both readers then read the NEW index through qpublish.newest without an error, and once they are gone the next reader puts it in place.
     PLANTED TWIN: the old publish (os.replace in place, retried, then raise) run against the same held file must FAIL with PermissionError.
  B  a stale index (codegen fingerprint changed) answers `q addr` AT ONCE with ONE `stale:` line and starts ONE background build; a second lookup meanwhile says the
     build is running and starts none; after it ends a lookup reads the new index with no stale line; a relinked map does not make index.db stale; host symbols come
     from their own file per map version (`q addr --host`), a held old one is no obstacle; a planted failed attempt for the current inputs is not retried (back-off line).
  D  qfact: a doc changes while a reader holds fact.db -> ONE rebuild, served from beside the held file; the NEXT call does not rebuild; once the reader is gone
     the copy is put in place.
  C  (S110 review) the background build is not in its starter's process tree, so q's watchdog (qtimeout.kill_children: psutil children(recursive=True))
     cannot kill it. PLANTED TWIN: a child started directly with DETACHED_PROCESS still names its starter as parent and IS in that tree.
  C3 (S110 review) a process the background build starts the way multiprocessing starts its Pool workers (creation flags 0) gets NO console window: a
     builder started with DETACHED_PROCESS has no console, so each of its workers opened a NEW console window on the screen (checked invisibly: the
     worker-like process is started with SW_HIDE and reports GetConsoleWindow()).
  K  (S110 review) the attempt record carries the key of the inputs the build READ: a codegen that lands after the build's last source check is not
     recorded as built (else every lookup printed "FAILED ... it finished, yet the index still reads stale" for 10 min instead of rebuilding).
  A8 (S110 review) the tools that open index.db by name (qnames.ro / stamp, qx7, qcopier) read the copy waiting beside a held index.db too.

    py -I tools\q\qpublish_selftest.py
"""
import contextlib
import io
import json
import os
import re
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import build_index  # noqa: E402
import qpublish  # noqa: E402

FAILS = []


def check(cond, msg, detail=''):
    print(('ok   ' if cond else 'FAIL ') + msg + ('' if cond or not detail else '\n     ' + str(detail)[:1500]))
    if not cond:
        FAILS.append(msg)


def stamp(gen, fp):
    with open(os.path.join(gen, 'codegen.stamp'), 'w', encoding='utf-8') as f:
        json.dump({'fingerprint': fp}, f)


def fixture(root, fp):
    """a generated/default of two functions (project prefix wwe2k14) and its codegen.stamp; -> the folder"""
    gen = os.path.join(root, 'generated', 'default')
    os.makedirs(gen, exist_ok=True)
    with open(os.path.join(gen, 'wwe2k14_init.cpp'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('PPCFuncMapping PPCFuncMappings[] = {\n    { 0x82000000, sub_82000000 },\n    { 0x82000010, sub_82000010 },\n};\n')
    with open(os.path.join(gen, 'wwe2k14_recomp.0.cpp'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('DEFINE_REX_FUNC(sub_82000000) {\n    // mflr r12\n    // bl 0x82000010\n    // blr\n}\n\n'
                'DEFINE_REX_FUNC(sub_82000010) {\n    // li r3,1\n    // blr\n}\n')
    stamp(gen, fp)
    return gen


def fake_map(path, n):
    """a linker map in MSVC's layout: one .text section, two guest functions and n extra symbols (n changes the size: a relink)"""
    lines = [' wwe2k14', '', f' Timestamp is 6543210{n} (fixture)', '', ' Preferred load address is 0000000140000000', '',
             ' Start         Length     Name                   Class', ' 0001:00000000 00001000H .text                   CODE', '',
             '  Address         Publics by Value              Rva+Base               Lib:Object', '',
             ' 0001:00000000       sub_82000000               0000000140001000 f   recomp.obj',
             ' 0001:00000010       sub_82000010               0000000140001010 f   recomp.obj']
    lines += [' 0001:%08x       extra_%d                    %016x f   extra.obj' % (0x20 + 4 * k, k, 0x140001020 + 4 * k) for k in range(n)]
    with open(path, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')


def run_build(gen, out, image):
    return subprocess.run([sys.executable, '-I', os.path.join(HERE, 'build_index.py'), '--gen', gen, '--image', image, '--out', out, '-j', '1'],
                          capture_output=True, text=True, timeout=300)


def fp_of(path):
    con = sqlite3.connect(f'file:{path}?mode=ro', uri=True)
    try:
        return json.loads(con.execute("SELECT value FROM meta WHERE key = 'codegen_fingerprint'").fetchone()[0])
    finally:
        con.close()


def old_replace_in_place(tmp, out, tries=60, wait=0.5):
    """PLANTED TWIN: build_index.py's publish before S110 QREBUILD (os.replace onto the live file, retried, then raise)"""
    for attempt in range(tries):
        try:
            os.replace(tmp, out)
            break
        except PermissionError:
            if attempt == tries - 1:
                raise
            time.sleep(wait)


def reader(db):
    """child mode: hold index.db open; on the parent's word read the newest index (qpublish.newest) while still holding the old handle"""
    con = sqlite3.connect(f'file:{db}?mode=ro', uri=True)
    v = json.loads(con.execute("SELECT value FROM meta WHERE key = 'codegen_fingerprint'").fetchone()[0])
    print(f'held {v}', flush=True)
    sys.stdin.readline()
    try:
        p = qpublish.newest(db)
        print(f'read {fp_of(p)} {os.path.basename(p)}', flush=True)
        rc = 0
    except Exception as e:  # noqa: BLE001 - reported to the parent
        print(f'error {e!r}', flush=True)
        rc = 1
    sys.stdin.readline()
    con.close()
    return rc


def wait_pid(pid, secs):
    try:
        import psutil
        psutil.Process(pid).wait(timeout=secs)
    except Exception:  # noqa: BLE001 - gone already, or the check after this one fails
        pass


def run_q(q, argv):
    o, e = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(o), contextlib.redirect_stderr(e):
        rc = q.main(argv)
    return rc, o.getvalue(), e.getvalue()


def case_a(tmp):
    root = os.path.join(tmp, 'a')
    idx = os.path.join(root, 'idx')
    os.makedirs(idx)
    gen = fixture(root, 'fpA')
    db, img = os.path.join(idx, 'index.db'), os.path.join(idx, 'image.bin')
    r = run_build(gen, db, img)
    check(r.returncode == 0 and fp_of(db) == 'fpA', 'A0 the fixture index builds (fingerprint fpA)', r.stdout + r.stderr)
    stamp(gen, 'fpB')
    readers = [subprocess.Popen([sys.executable, '-I', os.path.abspath(__file__), '--_reader', db], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
               for _ in range(2)]
    try:
        heads = [p.stdout.readline().strip() for p in readers]
        check(heads == ['held fpA'] * 2, 'A1 two reader processes hold index.db open (both read fpA)', heads)
        twin = db + '.twin.tmp'
        shutil.copyfile(db, twin)
        err = None
        try:
            old_replace_in_place(twin, db, tries=4, wait=0.25)
        except PermissionError as e:
            err = e
        check(err is not None, 'A2 PLANTED TWIN: the old replace-in-place onto the held index.db FAILS (PermissionError): the S110 fault', repr(err))
        if os.path.exists(twin):
            os.remove(twin)
        t0 = time.time()
        r = run_build(gen, db, img)
        dt = time.time() - t0
        pend = qpublish._pending(db)
        check(r.returncode == 0 and len(pend) == 1 and fp_of(db) == 'fpA' and fp_of(pend[0][1]) == 'fpB',
              'A3 a rebuild while two readers hold index.db exits 0; its index (fpB) waits beside the held file; index.db is untouched', r.stdout + r.stderr + str(pend))
        check(dt < 25, f'A4 the rebuild did not wait for the readers ({dt:.1f} s; the old publish retried 30 s and then failed)')
        check(not os.path.exists(db + '.lock') and (qpublish.last(db) or {}).get('result') == 'ok', 'A5 the build lock is released; the attempt record says ok', qpublish.last(db))
        for p in readers:
            p.stdin.write('go\n')
            p.stdin.flush()
        reads = [p.stdout.readline().strip() for p in readers]
        check(all(x.startswith('read fpB ') for x in reads), 'A6 both readers, still holding the old file, read the NEW index (fpB) without an error', reads)
    finally:
        for p in readers:
            try:
                p.stdin.write('done\n')
                p.stdin.flush()
            except OSError:
                pass
        rcs = [p.wait(timeout=60) for p in readers]
    got = qpublish.newest(db)
    check(rcs == [0, 0] and got == db and fp_of(db) == 'fpB' and not qpublish._pending(db),
          'A7 once the readers are gone the next reader puts the new index in place (index.db = fpB, no pending copy left)', (rcs, got))
    case_a8(db)


def case_a8(db):
    """the openers outside q.connect, against a fixture index.db held open with a newer copy beside it"""
    import qcopier
    import qnames
    import qx7
    gen = os.path.join(os.path.dirname(os.path.dirname(db)), 'generated', 'default')
    old = time.time() - 3600
    os.utime(db, (old, old))       # the held file's size:mtime stamp must differ from the new copy's (same fixture size, same second otherwise)
    hold = sqlite3.connect(f'file:{db}?mode=ro', uri=True)
    hold.execute('SELECT COUNT(*) FROM funcs').fetchone()
    saved = (qnames.INDEX_DB, os.environ.get('WWE_Q_INDEX'))
    try:
        stamp(gen, 'fpC')
        r = run_build(gen, db, os.path.join(os.path.dirname(db), 'image.bin'))
        pend = qpublish._pending(db)
        qnames.INDEX_DB = db
        os.environ['WWE_Q_INDEX'] = db
        got = {}
        c = qnames.ro(db)
        got['qnames.ro'] = json.loads(c.execute("SELECT value FROM meta WHERE key = 'codegen_fingerprint'").fetchone()[0])
        c.close()
        c = qx7._index_con()
        got['qx7'] = json.loads(c.execute("SELECT value FROM meta WHERE key = 'codegen_fingerprint'").fetchone()[0])
        c.close()
        st = os.stat(pend[-1][1]) if pend else None
        ix = qcopier.Index.open(db)
        got['qcopier'] = ix is not None and st is not None and ix.stat == '%d:%d' % (int(st.st_mtime), st.st_size)
        if ix:
            ix.con.close()
        got['stamp'] = st is not None and qnames.stamp() == '%d:%d' % (st.st_size, int(st.st_mtime))
        check(r.returncode == 0 and len(pend) == 1 and got['qnames.ro'] == 'fpC' and got['qx7'] == 'fpC' and got['qcopier'] and got['stamp'],
              'A8 qnames.ro / qnames.stamp / qx7 / qcopier read the copy beside a held index.db (fpC), not the held old file', (got, pend, r.stderr[-500:]))
    finally:
        hold.close()
        qnames.INDEX_DB = saved[0]
        if saved[1] is None:
            os.environ.pop('WWE_Q_INDEX', None)
        else:
            os.environ['WWE_Q_INDEX'] = saved[1]
    qpublish.newest(db)


def idle(secs):
    """child mode: stands for a background build (a process that runs a while)"""
    time.sleep(float(secs))
    return 0


def console_child(leaf):
    """child mode: stands for build_index.py starting a Pool worker (creation flags 0); the worker-like process is started HIDDEN (SW_HIDE) so a
    console it gets never shows, and says whether it got one"""
    import ctypes
    if leaf:
        print('worker console window', bool(ctypes.windll.kernel32.GetConsoleWindow()), flush=True)
        return 0
    si = subprocess.STARTUPINFO()
    si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    si.wShowWindow = 0
    r = subprocess.run([sys.executable, '-I', os.path.abspath(__file__), '--_console_leaf'], stdin=subprocess.DEVNULL, capture_output=True, text=True,
                       creationflags=0, startupinfo=si, timeout=60)
    print(r.stdout.strip(), flush=True)
    return 0


def case_c(tmp):
    import psutil
    import qtimeout
    log = os.path.join(tmp, 'c', 'build.log')
    cmd = [sys.executable, '-I', os.path.abspath(__file__), '--_idle', '20']
    twin = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, close_fds=True,
                            creationflags=subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP)
    kids = [k.pid for k in psutil.Process().children(recursive=True)]
    check(twin.pid in kids, 'C0 PLANTED TWIN: a child started directly with DETACHED_PROCESS is in its starter\'s process tree (the watchdog kills it)', (twin.pid, kids))
    twin.kill()
    twin.wait(10)
    pid = qpublish.start(cmd, log)
    try:
        kids = [k.pid for k in psutil.Process().children(recursive=True)]
        check(pid not in kids and qpublish.pid_alive(pid), 'C1 qpublish.start: the background build runs and is NOT in its starter\'s process tree', (pid, kids))
        qtimeout.kill_children()
        check(qpublish.pid_alive(pid), 'C2 the background build outlives its starter\'s watchdog kill (qtimeout.kill_children)', pid)
        pid3 = qpublish.start([sys.executable, '-I', os.path.abspath(__file__), '--_console'], log + '.3')
        wait_pid(pid3, 60)
        with open(log + '.3', encoding='utf-8', errors='replace') as f:
            said = f.read().strip()
        check(said.endswith('worker console window False'), 'C3 a worker the background build starts (flags 0, as multiprocessing does) gets no console window', said)
    finally:
        try:
            psutil.Process(pid).kill()
        except psutil.Error:
            pass


def case_k(tmp):
    root = os.path.join(tmp, 'k')
    idx = os.path.join(root, 'idx')
    os.makedirs(idx)
    gen = fixture(root, 'fpA')
    db, img = os.path.join(idx, 'index.db'), os.path.join(idx, 'image.bin')
    key_a = build_index.index_key(gen, img)
    real = qpublish.publish

    def publish_after_codegen(tmp_, path):
        stamp(gen, 'fpB')          # a codegen lands after the build's last source check, before it publishes and records
        return real(tmp_, path)
    saved = sys.argv
    sys.argv = ['build_index.py', '--gen', gen, '--image', img, '--out', db, '-j', '1']
    qpublish.publish = publish_after_codegen
    try:
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            build_index.main()
    finally:
        qpublish.publish = real
        sys.argv = saved
    rec = qpublish.last(db) or {}
    what = qpublish.kick(db, build_index.index_key(gen, img), ['unused'], os.path.join(idx, 'build_index.log'), spawn=False)
    check(rec.get('result') == 'ok' and rec.get('key') == key_a and fp_of(db) == 'fpA' and 'FAILED' not in what,
          'K1 the record names the inputs the build read (fpA), so the lookup after the codegen (fpB) is not backed off', (rec, key_a, what))


def case_b(tmp):
    import q
    import qhostmap
    root = os.path.join(tmp, 'b')
    idx = os.path.join(root, 'idx')
    os.makedirs(idx)
    gen = fixture(root, 'fpA')
    db, img = os.path.join(idx, 'index.db'), os.path.join(idx, 'image.bin')
    mp = os.path.join(root, 'wwe2k14.map')
    fake_map(mp, 1)
    r = run_build(gen, db, img)
    check(r.returncode == 0, 'B0 the fixture index builds', r.stdout + r.stderr)
    saved = (q.DB, build_index.MAP, os.environ.pop('WWE_Q_NO_AUTOUPDATE', None))
    q.DB, build_index.MAP = db, mp
    try:
        stamp(gen, 'fpB')
        t0 = time.time()
        rc, out, err = run_q(q, ['addr', '0x82000004', '--json'])
        dt = time.time() - t0
        m = re.search(r'rebuilding in the background \(pid (\d+)', err)
        warns = [x for x in err.splitlines() if x.startswith('q: WARNING')]
        ok = rc == 0 and json.loads(out).get('function') == 'sub_82000000'
        check(ok and m is not None and len(warns) == 1 and warns[0].startswith('q: WARNING stale: this answer is from the index built') and dt < 10,
              f'B1 a stale index answers AT ONCE ({dt:.1f} s) with ONE stale line and starts one background build', (rc, out, err))
        rc2, out2, err2 = run_q(q, ['addr', '0x82000004'])
        check(rc2 == 0 and ('a rebuild is running' in err2 or 'a rebuild was started' in err2) and 'rebuilding in the background' not in err2,
              'B2 a second lookup while the build runs answers from the old index, says the build is running and starts no second one', err2)
        if m:
            wait_pid(int(m.group(1)), 180)
        rc3, out3, err3 = run_q(q, ['addr', '0x82000004', '--json'])
        j3 = json.loads(out3) if rc3 == 0 else {}
        check(rc3 == 0 and 'stale' not in err3 and fp_of(qpublish.newest(db)) == 'fpB' and j3.get('host_rva_of_function') == '0x1000',
              'B3 after the background build a lookup reads the new index (no stale line), host RVA from the map', (err3, out3))
        rc4, out4, err4 = run_q(q, ['addr', '--host', 'wwe2k14.exe+0x1014', '--json'])
        j4 = json.loads(out4) if rc4 == 0 else {}
        check(j4.get('symbol') == 'sub_82000010' and j4.get('offset') == '0x4' and j4.get('guest_function') == 'sub_82000010',
              'B4 q addr --host answers from the host map file (sub_82000010 +0x4)', (out4, err4))
        con = sqlite3.connect(f'file:{db}?mode=ro', uri=True)
        mm = json.loads(con.execute("SELECT value FROM meta WHERE key = 'map'").fetchone()[0])
        con.close()
        held, info = qhostmap.open_map(idx)
        fake_map(mp, 3)
        rc5, out5, err5 = run_q(q, ['addr', '0x82000014', '--json'])
        j5 = json.loads(out5) if rc5 == 0 else {}
        files = sorted(os.path.basename(p) for p in os.listdir(idx) if p.startswith('hostmap.') and p.endswith('.db'))
        check(mm is None and rc5 == 0 and 'stale' not in err5 and j5.get('host_rva_of_function') == '0x1010' and len(files) == 2,
              'B5 a relink (the map changed) does not make index.db stale; the new map gets its OWN file while the old one is held open', (mm, err5, out5, files))
        held.close()
        stamp(gen, 'fpC')
        qpublish.record(db, build_index.index_key(gen, img), 'failed', note='planted failure')
        rc6, out6, err6 = run_q(q, ['addr', '0x82000004'])
        check(rc6 == 0 and 'FAILED' in err6 and 'planted failure' in err6 and (qpublish.last(db) or {}).get('result') == 'failed' and qpublish.holder(db) is None,
              'B6 a failed attempt for the same inputs is not retried by the next lookup: one loud line, no build started', err6)
    finally:
        q.DB, build_index.MAP = saved[0], saved[1]
        if saved[2] is not None:
            os.environ['WWE_Q_NO_AUTOUPDATE'] = saved[2]


def case_d(tmp):
    import qfact
    root = os.path.join(tmp, 'd')
    os.makedirs(os.path.join(root, 'docs', 'formats'))
    doc = os.path.join(root, 'docs', 'formats', 'zq.md')
    with open(doc, 'w', encoding='utf-8') as f:
        f.write('# Zq\n\nthe zqwidget format\n')
    saved = (qfact.ROOT, qfact.INDEX)
    qfact.use_root(root)
    try:
        con, info = qfact.connect()
        con.close()
        check(info['rebuilt_s'] is not None and os.path.exists(qfact.INDEX), 'D0 the library index builds')
        hold = sqlite3.connect(f'file:{qfact.INDEX}?mode=ro', uri=True)
        hold.execute('SELECT COUNT(*) FROM chunks').fetchone()
        with open(doc, 'w', encoding='utf-8') as f:
            f.write('# Zq\n\nthe zqwidget format, version two\n')
        con2, info2 = qfact.connect()
        hits = qfact.search(con2, ['version'], 5, None)['hits']
        con2.close()
        check(info2['rebuilt_s'] is not None and len(qpublish._pending(qfact.INDEX)) == 1 and len(hits) == 1,
              'D1 a doc changed while a reader holds fact.db: one rebuild, the new index served from beside the held file', (info2, hits))
        con3, info3 = qfact.connect()
        con3.close()
        check(info3['rebuilt_s'] is None, 'D2 the NEXT call reads that copy and does NOT rebuild again (before S110 every call rebuilt while the reader held fact.db)', info3)
        hold.close()
        con4, info4 = qfact.connect()
        con4.close()
        check(info4['rebuilt_s'] is None and not qpublish._pending(qfact.INDEX), 'D3 once the reader is gone the copy is put in place', info4)
    finally:
        qfact.ROOT, qfact.INDEX = saved


def main():
    if len(sys.argv) > 2 and sys.argv[1] == '--_reader':
        return reader(sys.argv[2])
    if len(sys.argv) > 2 and sys.argv[1] == '--_idle':
        return idle(sys.argv[2])
    if len(sys.argv) > 1 and sys.argv[1] in ('--_console', '--_console_leaf'):
        return console_child(sys.argv[1] == '--_console_leaf')
    tmp = tempfile.mkdtemp(prefix='qrebuild_')
    try:
        case_c(tmp)          # first: its watchdog kill takes every child of this process
        case_k(tmp)
        case_a(tmp)
        case_b(tmp)
        case_d(tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print(f'qpublish selftest: {"FAIL (" + str(len(FAILS)) + ")" if FAILS else "OK"}')
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
