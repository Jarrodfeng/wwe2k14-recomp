r"""Self-test of the schema-in-the-file-name rule for the vcall, runlog and sdk stores (S105 lane D, finding F1 of docs\research\FABLE_AUDIT_INDEX.md; the fields store is tested in
qfield_selftest.py). Modelled on qasset_selftest.py: a q of another schema never opens, wipes or rebuilds another schema's file.

Per store: a file of ANOTHER name / schema is planted in a TEMP index folder, a call of the current schema builds <store>.s<SCHEMA>.db beside it, the planted file stays
BYTE-IDENTICAL, and after a simulated schema bump (N -> N+1) the N file is byte-identical too and the N+1 file is built beside it. Nothing here touches out\harness\index of any
checkout: every store's folder is pointed at a temp folder (qx7.IDX, qrunlog.IDX, a temp root for qsdk).

    py -I tools/q/qindex_schema_selftest.py
"""
import hashlib
import os
import shutil
import sqlite3
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qrunlog  # noqa: E402
import qsdk  # noqa: E402
import qx7  # noqa: E402
import qx7_selftest  # noqa: E402


def sha(path):
    with open(path, 'rb') as f:
        return hashlib.sha256(f.read()).hexdigest()


def check(cond, msg, fails):
    if not cond:
        fails.append(msg)
        print('FAIL', msg)
    else:
        print('ok  ', msg)


def plant(path, text=b'ANOTHER SCHEMA'):
    with open(path, 'wb') as f:
        f.write(text * 40)
    return sha(path)


# ------------------------------------------------------------------------------------------------------------------------------- vcall (qx7.py)
def vcall_checks(fails):
    if not hasattr(qx7, 'db_name'):
        check(False, 'vcall: qx7.db_name() exists (the schema is in the file name)', fails)
        return
    tmp = tempfile.mkdtemp(prefix='qs_vcall_')
    saved = (qx7.IDX, qx7.GEN, qx7._index_con, qx7.VERSION)
    try:
        gen, con = qx7_selftest.build_fixture(tmp)
        idx = os.path.join(tmp, 'out', 'idx')
        os.makedirs(idx)
        qx7.IDX, qx7.GEN, qx7._index_con = idx, gen, (lambda: con)
        legacy = os.path.join(idx, 'vcall.db')
        h0 = plant(legacy)
        c = qx7.open_vdb()
        n1 = c.execute('SELECT COUNT(*) FROM vcall').fetchone()[0]
        c.close()
        mine = os.path.join(idx, qx7.db_name())
        check(qx7.db_name() == 'vcall.s%d.db' % qx7.VERSION and os.path.exists(mine), f'vcall: built as {qx7.db_name()}', fails)
        check(sha(legacy) == h0, 'vcall: the old-name vcall.db was left byte-identical by a schema-%d q call' % qx7.VERSION, fails)
        check('vcall.db' in ' '.join(qx7.other_index_files()), 'vcall: other_index_files names the older file', fails)
        h1 = sha(mine)
        qx7.VERSION += 1                                # a schema bump: N -> N+1
        c = qx7.open_vdb()
        n2 = c.execute('SELECT COUNT(*) FROM vcall').fetchone()[0]
        c.close()
        new = os.path.join(idx, qx7.db_name())
        check(new != mine and os.path.exists(new) and sha(mine) == h1 and sha(legacy) == h0,
              f'vcall: a schema-{qx7.VERSION} call built {qx7.db_name()} beside it; the schema-{qx7.VERSION - 1} file and the legacy file are byte-identical', fails)
        check(n1 == n2 and n1 > 0, f'vcall: the same rows from the new file ({n1} == {n2})', fails)
    finally:
        qx7.IDX, qx7.GEN, qx7._index_con, qx7.VERSION = saved
        shutil.rmtree(tmp, ignore_errors=True)


# ----------------------------------------------------------------------------------------------------------------------------- runlog (qrunlog.py)
def runlog_checks(fails):
    if not hasattr(qrunlog, 'db_name'):
        check(False, 'runlog: qrunlog.db_name() exists (the schema is in the file name)', fails)
        return
    tmp = tempfile.mkdtemp(prefix='qs_runlog_')
    saved = (qrunlog.IDX, qrunlog.VERSION)
    try:
        idx = os.path.join(tmp, 'idx')
        os.makedirs(idx)
        qrunlog.IDX = idx
        # an older-schema index under the old name, holding the row of a log that is GONE from disk (the tidy cleared out\): it must not be lost by the schema change
        legacy = os.path.join(idx, 'runlog.db')
        qrunlog.VERSION = saved[1] - 1
        con = qrunlog.update(legacy, logs=[], quiet=True)
        con.execute("INSERT INTO runs(path, size, mtime, run_number, exe, exe_built, exe_exists, script, flags, version, start, nlines) VALUES "
                    "('C:/gone/wwe2k14_001.log', 1, 1.0, 1, 'x.exe', '', 0, '', '', 'v', '2026-01-01 00:00:00', 1)")
        con.execute("INSERT INTO lines(text, run_id, lineno, kind, count, which) VALUES ('a line of a vanished run', 1, 1, 'census', 1, 'first')")
        con.close()
        qrunlog.VERSION = saved[1]
        h0 = sha(legacy)
        con = qrunlog.update(None, logs=[], quiet=True)
        mine = os.path.join(idx, qrunlog.db_name())
        gone = con.execute("SELECT COUNT(*) FROM runs WHERE path = 'C:/gone/wwe2k14_001.log'").fetchone()[0]
        txt = con.execute("SELECT COUNT(*) FROM lines WHERE text = 'a line of a vanished run'").fetchone()[0]
        con.close()
        check(qrunlog.db_name() == 'runlog.s%d.db' % qrunlog.VERSION and os.path.exists(mine), f'runlog: built as {qrunlog.db_name()}', fails)
        check(sha(legacy) == h0, 'runlog: the old-name runlog.db (schema %d) was left byte-identical by a schema-%d q call' % (qrunlog.VERSION - 1, qrunlog.VERSION), fails)
        check(gone == 1 and txt == 1, 'runlog: the row and lines of a log gone from disk were carried into the new file (the old file is only READ)', fails)
        check('runlog.db' in ' '.join(qrunlog.other_index_files()), 'runlog: other_index_files names the older file', fails)
        h1 = sha(mine)
        qrunlog.VERSION += 1                            # a schema bump: N -> N+1
        con = qrunlog.update(None, logs=[], quiet=True)
        new = os.path.join(idx, qrunlog.db_name())
        gone2 = con.execute("SELECT COUNT(*) FROM runs WHERE path = 'C:/gone/wwe2k14_001.log'").fetchone()[0]
        con.close()
        check(new != mine and os.path.exists(new) and sha(mine) == h1 and sha(legacy) == h0,
              f'runlog: a schema-{qrunlog.VERSION} call built {qrunlog.db_name()} beside it; the schema-{qrunlog.VERSION - 1} file and the legacy file are byte-identical', fails)
        check(gone2 == 1, 'runlog: the gone-log row travelled on to the newest file too', fails)
    finally:
        qrunlog.IDX, qrunlog.VERSION = saved
        shutil.rmtree(tmp, ignore_errors=True)


def runlog_same_version_checks(fails):
    """the real merge case: the old-name runlog.db IS the current version (7): the new runlog.s7.db is seeded from it (gone-log rows kept), the old file untouched"""
    if not hasattr(qrunlog, 'db_name'):
        check(False, 'runlog: qrunlog.db_name() exists', fails)
        return
    tmp = tempfile.mkdtemp(prefix='qs_runlog7_')
    saved = qrunlog.IDX
    try:
        idx = os.path.join(tmp, 'idx')
        os.makedirs(idx)
        qrunlog.IDX = idx
        legacy = os.path.join(idx, 'runlog.db')
        con = qrunlog.update(legacy, logs=[], quiet=True)           # version = the current one
        con.execute("INSERT INTO runs(path, size, mtime, run_number, exe, exe_built, exe_exists, script, flags, version, start, nlines) VALUES "
                    "('C:/gone/wwe2k14_007.log', 1, 1.0, 7, 'x.exe', '', 0, '', '', 'v', '2026-01-01 00:00:00', 1)")
        con.execute("INSERT INTO lines(text, run_id, lineno, kind, count, which) VALUES ('same version vanished run', 1, 1, 'census', 1, 'first')")
        con.close()
        h0 = sha(legacy)
        con = qrunlog.update(None, logs=[], quiet=True)
        gone = con.execute("SELECT COUNT(*) FROM runs WHERE path = 'C:/gone/wwe2k14_007.log'").fetchone()[0]
        txt = con.execute("SELECT COUNT(*) FROM lines WHERE text = 'same version vanished run'").fetchone()[0]
        con.close()
        check(os.path.exists(os.path.join(idx, qrunlog.db_name())) and gone == 1 and txt == 1, 'runlog: a same-version legacy runlog.db seeds runlog.s%d.db with the gone-log rows' % qrunlog.VERSION, fails)
        check(sha(legacy) == h0, 'runlog: the same-version legacy file stayed byte-identical', fails)
        # qx7.other_index_files(path) names the files beside the path it is given, not beside the default folder
        other = os.path.join(tmp, 'o')
        os.makedirs(other)
        plant(os.path.join(other, 'vcall.db'))
        plant(os.path.join(other, 'vcall.s9.db'))
        check(qx7.other_index_files(os.path.join(other, 'vcall.s9.db')) == ['vcall.db'], 'vcall: other_index_files(path) lists the folder of the given path', fails)
    finally:
        qrunlog.IDX = saved
        shutil.rmtree(tmp, ignore_errors=True)


# ---------------------------------------------------------------------------------------------------------------------------------- sdk (qsdk.py)
def sdk_checks(fails):
    tmp = tempfile.mkdtemp(prefix='qs_sdk_')
    saved = qsdk.SCHEMA
    try:
        sdk = os.path.join(tmp, 'thirdparty', 'rexglue-sdk-src')
        os.makedirs(os.path.join(sdk, 'src'))
        with open(os.path.join(sdk, 'src', 'a.cpp'), 'w') as f:
            f.write('void PlantedSdkFn() {}\nvoid Caller() { PlantedSdkFn(); }\n')
        idx = os.path.join(tmp, 'out', 'harness', 'index')
        os.makedirs(idx)
        legacy = os.path.join(idx, 'sdk.db')
        h0 = plant(legacy)
        h5 = plant(os.path.join(idx, 'sdk.s5.db'), b'SCHEMA FIVE')
        mine = qsdk.db_path(tmp)
        check(os.path.basename(mine) == 'sdk.s%d.db' % qsdk.SCHEMA, f'sdk: the default index is {os.path.basename(mine)}', fails)
        check(os.path.basename(qsdk.db_path(tmp, os.path.join('x', 'wt1'))) == 'sdk-wt1.s%d.db' % qsdk.SCHEMA, 'sdk: a worktree SDK index is sdk-<name>.s<SCHEMA>.db', fails)
        state, _n, _s = qsdk.ensure_fresh(tmp)
        check(state == 'built' and os.path.exists(mine), f'sdk: built {os.path.basename(mine)} (state {state})', fails)
        check(sha(legacy) == h0 and sha(os.path.join(idx, 'sdk.s5.db')) == h5, 'sdk: the old-name sdk.db and an older sdk.s5.db were left byte-identical', fails)
        qsdk.connect(tmp).close()
        check('left alone' not in qsdk.HEADER, 'sdk: the per-answer header does NOT carry the other-schema file names (they are named in info only)', fails)
        import contextlib
        import io
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            qsdk.main(['info', '--root', tmp])
        check('other_index_files' in buf.getvalue() and 'sdk.db' in buf.getvalue(), 'sdk: `info` names the other-schema files it left alone', fails)
        os.environ['WWE_QSDK_DB'] = os.path.join(tmp, 'elsewhere.db')
        try:
            check(qsdk.db_path(tmp) == os.path.join(tmp, 'elsewhere.db'), 'sdk: WWE_QSDK_DB points the index at another file (a builder / a gate lane never touches the shared one)', fails)
        finally:
            del os.environ['WWE_QSDK_DB']
        h1 = sha(mine)
        qsdk.SCHEMA += 1                                # a schema bump: N -> N+1
        state, _n, _s = qsdk.ensure_fresh(tmp)
        new = qsdk.db_path(tmp)
        check(new != mine and os.path.exists(new) and sha(mine) == h1 and sha(legacy) == h0,
              f'sdk: a schema-{qsdk.SCHEMA} call built {os.path.basename(new)} beside it; the schema-{qsdk.SCHEMA - 1} file and the legacy file are byte-identical', fails)
        c = sqlite3.connect(new)
        n = c.execute("SELECT COUNT(*) FROM defs WHERE name = 'PlantedSdkFn'").fetchone()[0]
        c.close()
        check(n == 1, 'sdk: the new file answers (the planted function is defined once)', fails)
    finally:
        qsdk.SCHEMA = saved
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    fails = []
    vcall_checks(fails)
    runlog_checks(fails)
    runlog_same_version_checks(fails)
    sdk_checks(fails)
    print('PASS' if not fails else f'FAIL ({len(fails)})')
    return 1 if fails else 0


if __name__ == '__main__':
    import multiprocessing
    multiprocessing.freeze_support()
    sys.exit(main())
