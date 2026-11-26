r"""Self-test of the schema-in-the-file-name rule for the vcall and sdk stores (S105 lane D, finding F1 of docs\research\FABLE_AUDIT_INDEX.md; the fields store is tested in
qfield_selftest.py). Modelled on qasset_selftest.py: a q of another schema never opens, wipes or rebuilds another schema's file.

Per store: a file of ANOTHER name / schema is planted in a TEMP index folder, a call of the current schema builds <store>.s<SCHEMA>.db beside it, the planted file stays
BYTE-IDENTICAL, and after a simulated schema bump (N -> N+1) the N file is byte-identical too and the N+1 file is built beside it. Nothing here touches out\harness\index of any
checkout: every store's folder is pointed at a temp folder (qx7.IDX, a temp root for qsdk).

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
        # qx7.other_index_files(path) names the files beside the path it is given, not beside the default folder (S111: kept from the deleted runlog block)
        other = os.path.join(tmp, 'o')
        os.makedirs(other)
        plant(os.path.join(other, 'vcall.db'))
        plant(os.path.join(other, 'vcall.s9.db'))
        check(qx7.other_index_files(os.path.join(other, 'vcall.s9.db')) == ['vcall.db'], 'vcall: other_index_files(path) lists the folder of the given path', fails)
    finally:
        qx7.IDX, qx7.GEN, qx7._index_con, qx7.VERSION = saved
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
            check(qsdk.db_path(tmp) == os.path.join(tmp, 'elsewhere.db'), 'sdk: WWE_QSDK_DB points the index at another file (a builder or a selftest never touches the shared one)', fails)
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
    sdk_checks(fails)
    print('PASS' if not fails else f'FAIL ({len(fails)})')
    return 1 if fails else 0


if __name__ == '__main__':
    import multiprocessing
    multiprocessing.freeze_support()
    sys.exit(main())
