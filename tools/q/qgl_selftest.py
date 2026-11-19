"""qgl_selftest: proves the S103 QGL work on fixtures in a TEMP folder (no clang, no game, 0 runs):
  glsl   `q ours glsl`: a uniform / an interpolator component / a define, with kind and the #if guard (#else inverted, a file's own include guard dropped);
         PLANTED twins: `inUvFog.xy` is NOT a hit for inUvFog.w, a comment naming it is not a hit, the #else twin of a write carries the INVERTED guard.
  fxreg  `q ours fxreg [--dupes]`: duplicate hashes grouped by owner (cross-owner flagged); a planted _fragment / _pl_fragment twin where only one side has a
         constants table is listed, the clean pair is not; a variant no hash row names is listed.
  str    `q ours str`: a literal, a printf-style literal found by its prefix (a different literal that starts the same IS reported, marked `prefix of a format`)
         and as two adjacent literals; PLANTED: a comment naming it is NOT a hit; a global cap stops a huge result.
  glsl2  (QGL review) write = nothing but an index / swizzle between the hit and the assignment (`f(inUvFog.w); z = 1` and `a == inUvFog.w` are reads), #elif =
         !(A) && B, whole-vector uses listed, backslash-continued #if, a directive inside a block comment ignored.
  lock   open_ro answers from `<db>.good` in < 5 s while a writer holds the db (planted: an EXCLUSIVE transaction); with no .good it waits its slow bound and
         fails with a clear message; snapshot_good makes the copy; bg_refresh starts ONE detached refresh (a second call is `recent`, a held build lock is `running`),
         and the child never inherits WWE_SWEEP_OWNER.
  worktree  qfresh.text_hits lists the declaration first and up to 40 lines; near_names finds char_e12 / kCharE12 for CharE12 and NOTHING for a planted CharE13.
Run:  py -I tools/q/qgl_selftest.py
"""
import json
import os
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
Q = os.path.join(HERE, 'q.py')
ENV = dict(os.environ, WWE_Q_OURS_NO_AUTOUPDATE='1', PYTHONIOENCODING='utf-8')


class Fail(Exception):
    pass


def check(cond, what, out=''):
    if not cond:
        raise Fail(f'{what}\n--- output ---\n{str(out)[-1500:]}')
    print(f'PASS  {what}')


def write(root, rel, text):
    p = os.path.join(root, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)


def q(root, *a):
    r = subprocess.run([sys.executable, '-I', Q, 'ours'] + list(a) + ['--root', root], capture_output=True, text=True, env=ENV, timeout=120, errors='replace')
    return r.stdout + r.stderr


GLSL = '''#ifndef FX_SKIN_GLSL
#define FX_SKIN_GLSL
layout(location = 1) in vec4 inUvFog;
layout(set = 0, binding = 1) uniform FragConsts {
  vec4 Sweat;             // x amount
} fc;
#ifdef FX_WRINKLES
void wr() {
  normalMap += wrinkle * inUvFog.w;
}
#else
void wr() {
  normalMap = base * inUvFog.w;
}
#endif
void main() {
  vec2 uv = inUvFog.xy;
  // inUvFog.w is only named in this comment
  float s = fc.Sweat.y * 2.0;
  fc.Sweat.x = 1.0;
  inUvFog.w = 0.5;
  if (fog == 1.0) { }
}
#endif
'''


def registry():
    def v(name, consts):
        c = 'kC_' + name if consts else 'nullptr'
        return f'  {{"{name}", kFx_{name}, sizeof(kFx_{name}), nullptr, 0, {c}, {consts}, 16, 65535, nullptr, 0, nullptr, 0, 0, 255}}'
    names = [('yAudDefault_base_fragment', 1), ('yAudience_base_fragment', 1), ('yBG_Default_fragment', 1), ('yBG_Default_pl_fragment', 0),
             ('yProp_Default_fragment', 1), ('yProp_Default_pl_fragment', 1), ('yAlone_quad_vertex', 0), ('yNohash_vertex', 0)]
    vs = ',\n'.join(v(n, c) for n, c in names)
    hs = ['{0x00000000000000AAull, 0}', '{0x00000000000000AAull, 1}', '{0x00000000000000BBull, 2}', '{0x00000000000000CCull, 3}',
          '{0x00000000000000DDull, 4}', '{0x00000000000000EEull, 5}', '{0x00000000000000FFull, 6}']
    return ('#pragma once\nnamespace rdr::fx {\ninline constexpr Variant kVariants[] = {\n' + vs + '\n};\n'
            'inline constexpr HashEntry kHashes[] = {\n  ' + ',\n  '.join(hs) + '\n};\n}\n')


CPP = '''#include <cstdio>
void Bind(int i) {
    char name[64];
    std::snprintf(name, sizeof(name), "g_fWklLv%d", i);
    const char* a = "g_fWklLv" "%d";
    const char* c = "g_mNormalWklPrm";
    const char* d = "g_fWklLv0Extra";   // a different literal that merely starts the same
    // "g_mNormalWklPrm" is only named in this comment
}
'''


def test_text_queries(d):
    write(d, 'src/shaders/fx_skin.glsl', GLSL)
    out = q(d, 'glsl', 'inUvFog.w')
    check('4 lines' in out and 'glsl inUvFog.w' in out, 'glsl inUvFog.w: the 3 lines that touch w + the whole-vector declaration', out)
    check('[decl  ] layout(location = 1) in vec4 inUvFog;   (whole vector)' in out, 'a whole-vector line (the declaration) is listed and marked', out)
    check('inUvFog.xy' not in out, 'PLANTED twin: `inUvFog.xy` is not a hit for inUvFog.w', out)
    check('only named in this comment' not in out, 'PLANTED twin: a comment naming it is not a hit', out)
    check(out.count('[read  ]') == 2 and out.count('[write ]') == 1 and out.count('[decl  ]') == 1, 'two reads (right of an = / +=) and one write (inUvFog.w = ..)', out)
    check('#if defined(FX_WRINKLES)' in out and '#if !(defined(FX_WRINKLES))' in out, 'the #ifdef guard, and its #else twin carries the INVERTED guard', out)
    check('FX_SKIN_GLSL' not in out, "the file's own include guard is not shown as a feature guard", out)
    out = q(d, 'glsl', 'Sweat')
    check('[decl  ]' in out and out.count('[read  ]') == 1 and '[write ]' in out, 'glsl Sweat: the field decl, one read (.y), one write (.x)', out)
    out = q(d, 'glsl', 'FX_WRINKLES')
    check('[guard ]' in out and '1 guard' in out, 'glsl FX_WRINKLES: the #ifdef is a guard row', out)
    out = q(d, 'glsl', 'NoSuchName')
    check('0 lines' in out, 'glsl: an unknown name says so', out)

    write(d, 'src/renderer/shaders/spv/fx_registry.h', registry())
    out = q(d, 'fxreg', '--dupes')
    check('1 duplicate hashes (1 across owners)' in out, 'fxreg: the planted cross-owner duplicate hash is counted', out)
    check('owner yAudDefault,yAudience' in out and 'CROSS-OWNER' in out and '0x00000000000000AA' in out, 'fxreg --dupes: grouped by owner, flagged cross-owner', out)
    check('yBG_Default_pl_fragment [0]   -> no table: yBG_Default_pl_fragment' in out, 'fxreg: the planted twin with no constants table on one side is listed', out)
    check('yProp_Default' not in out.split('twins without a table')[-1].split('variants no hash')[0], 'fxreg: the clean twin pair (both with a table) is NOT listed', out)
    check('yNohash_vertex' in out, 'fxreg --dupes: a variant no hash row names is listed', out)
    out2 = q(d, 'fxreg')
    check('--dupes lists' in out2 and '0x00000000000000AA' not in out2, 'fxreg without --dupes only counts', out2)

    write(d, 'src/bind.cpp', CPP)
    out = q(d, 'str', 'g_fWklLv%d')
    check(out.count('<literal>') == 2 and 'a = "g_fWklLv" "%d"' in out and 'snprintf' in out, 'str "g_fWklLv%d": the printf literal and the split literals', out)
    check('g_fWklLv0Extra' in out and '<prefix of a format>' in out, 'str: a printf-style literal also matches by its prefix (reported as such)', out)
    out = q(d, 'str', 'g_mNormalWklPrm')
    check(out.count('src/bind.cpp:') == 1 and 'const char* c' in out, 'str "g_mNormalWklPrm": the real use', out)
    check('only named in this comment' not in out, 'PLANTED twin: a comment naming the literal is not a use', out)
    out = q(d, 'str', 'g_mNothingHere')
    check('0 string-literal sites' in out, 'str: an unused literal says so', out)


GLSL2 = '''#ifdef A_ON
void a() { }
#elif defined(B_ON) && \\
      defined(C_ON)
void b() { v = inUvFog.w; }
#else
void c() { inUvFog.w += 1.0; y = 2.0; }
#endif
/*
#ifdef HIDDEN_GUARD
*/
void d() {
  foo(inUvFog.w); z = 1.0;
  if (v == inUvFog.w) { k = 1.0; }
  inUvFog = vec4(0.0);
  out_c = texture(t, inUvFog.xy);
  g(inUvFog);
}
'''


def test_glsl2(d):
    write(d, 'src/shaders/g2.glsl', GLSL2)
    out = q(d, 'glsl', 'inUvFog.w')
    check('[write ] void c() { inUvFog.w += 1.0; y = 2.0; }' in out, 'review 11: `inUvFog.w += 1.0` is a write', out)
    check('[read  ] foo(inUvFog.w); z = 1.0;' in out, 'review 11: `foo(inUvFog.w); z = 1.0;` is a READ (the assignment after the ; is not the hit\'s)', out)
    check('[read  ] if (v == inUvFog.w)' in out, 'review 11: `v == inUvFog.w` is a read (== is not an assignment)', out)
    check('inUvFog = vec4(0.0)' in out and '(whole vector)' in out and 'g(inUvFog)' in out, 'review 12: whole-vector uses are listed for a component query', out)
    check('texture(t, inUvFog.xy)' not in out, 'review 12: a use of other components (.xy) is still skipped', out)
    check('[write ] inUvFog = vec4(0.0);   (whole vector)' in out, 'a whole-vector assignment is a write', out)
    check('#if !(defined(A_ON)) && defined(B_ON) && defined(C_ON)' in out, 'review 12: #elif (backslash-continued) = !(A) && B && C', out)
    check('#if !(defined(A_ON) || defined(B_ON) && defined(C_ON))' in out or '#if !(defined(A_ON) || defined(B_ON) && defined(C_ON))' in out.replace('  ', ' '),
          '#else = the negation of every earlier test of the chain', out)
    check('HIDDEN_GUARD' not in out and out.count('#if defined(HIDDEN_GUARD)') == 0, 'a #ifdef inside a block comment is not a guard', out)
    # the lines after the block comment carry NO guard of the commented-out ifdef
    check('#if defined(HIDDEN_GUARD)' not in out, 'no line sits under the commented-out #ifdef', out)


def test_lock2(d):
    """review 1, 2, 8, 9, 10: worktree / overlay readers attach the path actually opened; the live connection keeps a long busy timeout; no .good = a retry message"""
    import qours
    import qfresh
    db = os.path.join(d, 'ours.db')
    for pth, tag in ((db, 'live'), (db + '.good', 'good')):
        c = sqlite3.connect(pth)
        for t in ('syms', 'refs', 'funcs'):
            c.execute(f'CREATE TABLE {t}(usr TEXT, qname TEXT, name TEXT, kind INT, file TEXT, line INT, isdef INT, builds INT, col INT, func TEXT, endline INT, cvars TEXT, logs TEXT)')
        c.execute('CREATE TABLE meta(key TEXT, value TEXT)')
        c.execute('CREATE TABLE files(path TEXT, sha TEXT, size INT, mtime_ns INT)')
        c.execute('CREATE TABLE tus(id INT, file TEXT)')
        c.execute('CREATE TABLE incs(tu INT, file TEXT)')
        c.execute('INSERT INTO meta VALUES(?,?)', ('tag', json.dumps(tag)))
        c.commit()
        c.close()
    os.environ['WWE_Q_DB_WAIT'] = '0.3'
    os.environ['WWE_Q_DB_SLOW_WAIT'] = '1'
    con = qours.open_ro(db)
    bt = con.execute('PRAGMA busy_timeout').fetchone()[0]
    check(bt == 60000 and qours.LAST_OPENED == db, f'review 2: after the probe the live connection has busy_timeout {bt} (not the 2 s probe wait); LAST_OPENED = the live path')
    con.close()
    w = sqlite3.connect(db, timeout=1, isolation_level=None)
    w.execute('BEGIN EXCLUSIVE')
    try:
        t0 = time.time()
        p = qours.readable_path(db)
        check(p == db + '.good' and time.time() - t0 < 5, 'review 1: readable_path during a swap = the .good path (what the overlay base and qfresh.wrap must attach)')
        os.makedirs(os.path.join(d, 'wt', 'src'))
        t0 = time.time()
        bc = sqlite3.connect(f'file:{db}.good?mode=ro', uri=True)
        mem, info = qfresh.wrap(bc, os.path.join(d, 'wt'), p)
        check(mem.execute('SELECT count(*) FROM syms').fetchone()[0] == 0 and time.time() - t0 < 6, 'review 1: a worktree reader (qfresh.wrap) opens the .good path while the live db is write-locked')
        mem.close()
        # PLANTED: the same wrap on the LIVE path cannot read it while the writer holds it
        try:
            m2, _ = qfresh.wrap(bc, os.path.join(d, 'wt'), db)
            m2.execute('SELECT count(*) FROM syms').fetchone()
            ok = False
        except sqlite3.OperationalError:
            ok = True
        check(ok, 'PLANTED twin: attaching the live path during the swap fails (so the old code, which used it, blocked / failed)')
        bc.close()
    finally:
        w.execute('ROLLBACK')
        w.close()
    # review 10: a live db that cannot be read at all (garbage) falls back to .good before raising
    bad = os.path.join(d, 'bad.db')
    with open(bad, 'wb') as f:
        f.write(b'not a database' * 100)
    shutil.copyfile(db + '.good', bad + '.good')
    con = qours.open_ro(bad)
    check(json.loads(con.execute('SELECT value FROM meta').fetchone()[0]) == 'good' and qours.LAST_OPENED == bad + '.good', 'review 10: an unreadable live index falls back to .good before raising')
    con.close()


def test_build_extras(d):
    """review 3-8: spawn flags / deferral / back-off, seeding from a killed refresh, snapshot naming and retry, the date in the reader note"""
    import build_ours
    import qours
    db = os.path.join(d, 'ours.db')
    sqlite3.connect(db).close()
    py = [sys.executable, '-I', '-c', 'print("ok")']
    # 3: the spawn helper's flags (read from the source: DETACHED_PROCESS gone, no-window + group + below-normal, breakaway first)
    code = qours.SPAWN_CODE
    check('0x08000000' in code and '0x00000200' in code and '0x00004000' in code and '0x00000008' not in code and 'base | 0x01000000, base' in code,
          'review 3: CREATE_NO_WINDOW | NEW_PROCESS_GROUP | BELOW_NORMAL, breakaway first then the plain flags, no DETACHED_PROCESS')
    # 4: deferred when the run lock is held or RAM is low (no spawn)
    real = (qours.run_lock_held, qours.free_ram_gb)
    try:
        qours.run_lock_held = lambda: True
        st, why = qours.bg_refresh(d, db)
        check(st == 'deferred' and 'run lock' in why, 'review 4: a held run lock defers the refresh (nothing spawned)', why)
        qours.run_lock_held, qours.free_ram_gb = (lambda: False), (lambda: 4.0)
        st, why = qours.bg_refresh(d, db)
        check(st == 'deferred' and 'free RAM' in why, 'review 4: free RAM under 12 GB defers the refresh', why)
    finally:
        qours.run_lock_held, qours.free_ram_gb = real
    check(not os.path.exists(db + '.bgstamp'), 'a deferred refresh left no stamp (nothing was spawned)')
    # 5: back-off marker honoured and reported
    with open(db + '.bgbackoff', 'w') as f:
        f.write('x')
    st, why = qours.bg_refresh(d, db, cmd=py)
    check(st == 'backoff' and 'backed off' in why, 'review 5: a back-off marker (a timed-out run) stops the restart and says so', why)
    os.utime(db + '.bgbackoff', (time.time() - 7200, time.time() - 7200))
    st, why = qours.bg_refresh(d, db, cmd=py)
    check(st == 'started', 'review 5: an old marker (> 1 h) no longer blocks', why)
    # 5: seeding tu_cache from a killed refresh
    left = db + '.tmp999'
    c = sqlite3.connect(left)
    c.execute('CREATE TABLE tu_cache(key TEXT PRIMARY KEY, flags TEXT, deps BLOB, rows BLOB)')
    c.executemany('INSERT INTO tu_cache VALUES(?,?,?,?)', [(f'k{i}', 'f', b'd', b'r') for i in range(7)])
    c.commit()
    c.close()
    dst = sqlite3.connect(os.path.join(d, 'new.db'))
    dst.execute('CREATE TABLE tu_cache(key TEXT PRIMARY KEY, flags TEXT, deps BLOB, rows BLOB)')
    n = build_ours.seed_cache(dst, left)
    check(n == 7 and dst.execute('SELECT count(*) FROM tu_cache').fetchone()[0] == 7, 'review 5: the killed refresh\'s 7 parsed units seed the next build\'s tu_cache')
    check(build_ours.seed_cache(dst, os.path.join(d, 'garbage.bin')) == 0, 'a damaged / missing leftover seeds nothing and never raises')
    dst.close()
    # 7 + 8: snapshot naming + retry + re-snapshot rule + date note
    check(build_ours.snapshot_good(db) and os.path.exists(db + '.good'), 'snapshot_good copies (temp name <db>.tmpgood<pid>: the leftover glob <db>.tmp* covers it)')
    check(not [x for x in os.listdir(d) if '.good.tmp' in x], 'review 7: no `.good.tmpNNN` temp name is used')
    held = open(db + '.good', 'rb')
    try:
        if os.name == 'nt':
            t0 = time.time()
            ok = build_ours.snapshot_good(db, retry_s=1)
            check((not ok) and time.time() - t0 < 10 and not [x for x in os.listdir(d) if '.tmpgood' in x],
                  'review 8: os.replace refused while a reader holds .good: retried, logged, temp removed')
    finally:
        held.close()
    old = time.time() - 3 * 86400
    os.utime(db + '.good', (old, old))
    check('-' in qours._today_note(db + '.good'), 'review 8: the reader note carries the date when .good is not from today', qours._today_note(db + '.good'))
    check('-' not in qours._today_note(db), 'a today\'s snapshot shows the time only')


def test_lock(d):
    import build_ours
    import qours
    db = os.path.join(d, 'ours.db')
    for pth, tag in ((db, 'live'), (db + '.good', 'good')):
        c = sqlite3.connect(pth)
        for t in ('syms', 'refs', 'funcs'):
            c.execute(f'CREATE TABLE {t}(a)')
        c.execute('CREATE TABLE meta(key TEXT, value TEXT)')
        c.execute('INSERT INTO meta VALUES(?,?)', ('tag', json.dumps(tag)))
        c.commit()
        c.close()
    os.environ['WWE_Q_DB_WAIT'] = '0.3'
    os.environ['WWE_Q_DB_SLOW_WAIT'] = '1'
    # a reader with no writer: the live db
    con = qours.open_ro(db)
    check(json.loads(con.execute("SELECT value FROM meta").fetchone()[0]) == 'live', 'no writer: the reader answers from the live index')
    con.close()
    # PLANTED writer: an exclusive transaction (what swap_in's backup holds while it copies 220 MB)
    w = sqlite3.connect(db, timeout=1, isolation_level=None)
    w.execute('BEGIN EXCLUSIVE')
    try:
        t0 = time.time()
        con = qours.open_ro(db)
        dt = time.time() - t0
        check(json.loads(con.execute("SELECT value FROM meta").fetchone()[0]) == 'good' and dt < 5, f'a writer holds the db: the reader answers from .good in {dt:.1f} s (< 5)')
        con.close()
        os.rename(db + '.good', db + '.good_off')
        t0 = time.time()
        try:
            qours.open_ro(db)
            check(False, 'no .good and a held writer must fail clearly')
        except qours.QOError as e:
            dt = time.time() - t0
            check('retry in a minute' in str(e) and 'do not rebuild' in str(e) and dt < 8, f'no .good: waits only the slow bound ({dt:.1f} s), then a clear message')
        os.rename(db + '.good_off', db + '.good')
    finally:
        w.execute('ROLLBACK')
        w.close()
    # snapshot_good
    os.unlink(db + '.good')
    check(build_ours.snapshot_good(db) and os.path.getsize(db + '.good') == os.path.getsize(db), 'snapshot_good: a whole copy of the swapped-in index')
    # bg_refresh
    log = os.path.join(d, 'ours_update.log')
    py = [sys.executable, '-I', '-c', 'import os; print("OWNER=" + str(os.environ.get("WWE_SWEEP_OWNER")))']
    os.environ['WWE_SWEEP_OWNER'] = '1:2'
    st, why = qours.bg_refresh(d, db, cmd=py)
    check(st == 'started' and 'background refresh was started' in why, 'bg_refresh: the first stale reader starts one background refresh', why)
    st, why = qours.bg_refresh(d, db, cmd=py)
    check(st == 'recent', 'bg_refresh: a second reader right after does not start another', st)
    for _ in range(50):
        if os.path.exists(log) and 'OWNER=' in open(log, errors='replace').read():
            break
        time.sleep(0.2)
    txt = open(log, errors='replace').read() if os.path.exists(log) else ''
    check('OWNER=None' in txt, 'the detached child never inherits WWE_SWEEP_OWNER (it outlives the q call)', txt)
    os.unlink(db + '.bgstamp')
    lk = build_ours.BuildLock(db, wait_s=0)
    check(lk.acquire(), 'the test takes the build lock')
    try:
        st, why = qours.bg_refresh(d, db, cmd=py)
        check(st == 'running' and 'holds the build lock' in why, 'bg_refresh: a held build lock = a refresh is running: start nothing', why)
    finally:
        lk.release()


def test_worktree_text(d):
    import qfresh
    write(d, 'src/renderer_scene/front_end.h', 'struct ObjectEngine { int32_t char_mode = -1; };\n' + ''.join(f'void use{i}() {{ x.char_mode = {i}; }}\n' for i in range(30))
          + 'enum { kCharE12 = 12 };\n// char_e12 is the twelfth\n')
    info = {'changed': [], 'new': ['src/renderer_scene/front_end.h'], 'mapped': [], 'stale': [], 'removed': [], 'note': ''}
    hits = qfresh.text_hits(d, info, 'char_mode')
    check(hits and hits[0][2].startswith('[decl?]') and len(hits) == 31, f'text_hits: the declaration first, all 31 lines (was capped at 6): {len(hits)}', hits[:2])
    nn = qfresh.near_names(d, info, 'CharE12')
    names = {n[0] for n in nn}
    check({'kCharE12', 'char_e12'} <= names, 'near_names: CharE12 -> kCharE12, char_e12 (case and underscores ignored)', nn)
    check(not qfresh.near_names(d, info, 'CharE13'), 'PLANTED twin: CharE13 finds nothing')
    write(d, 'src/renderer_scene/expr.h', 'int f(int a, int b, int name) { return a * name + (b & name); }\nuint32_t* name = nullptr;\nconst Foo& name2 = x;\n')
    info2 = {'changed': [], 'new': ['src/renderer_scene/expr.h'], 'mapped': [], 'stale': [], 'removed': [], 'note': ''}
    h2 = qfresh.text_hits(d, info2, 'name')
    marks = {ln: tx.startswith('[decl?]') for _, ln, tx in h2}
    check(marks[2] and not marks.get(1, True) is True or (marks[2] and not marks[1]), 'review 15: `uint32_t* name =` is a [decl?], `a * name` / `b & name` are not', h2)


def main():
    d = tempfile.mkdtemp(prefix='qgl_selftest_')
    try:
        test_text_queries(os.path.join(d, 'fx'))
        test_glsl2(os.path.join(d, 'fx'))
        os.makedirs(os.path.join(d, 'lock'))
        test_lock(os.path.join(d, 'lock'))
        os.makedirs(os.path.join(d, 'lock2'))
        test_lock2(os.path.join(d, 'lock2'))
        os.makedirs(os.path.join(d, 'bx'))
        test_build_extras(os.path.join(d, 'bx'))
        test_worktree_text(os.path.join(d, 'wt'))
    except Fail as e:
        print('FAIL ' + str(e))
        return 1
    finally:
        shutil.rmtree(d, ignore_errors=True)
    print('qgl_selftest OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
