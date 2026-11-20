"""Selftest for qbody (body / outline / enclosing), the qfresh sha cache, `q py body` and `q note` (S105 QC + fix round). Planted twins: each check can say
PASS wrongly in the unsafe direction (a cache that keeps a stale hash, a scan that counts a prototype or a brace in a string, an enclosing answer for a line
outside any function, an `[index]` label on lines the index did not see). Each twin was checked to FAIL when its fault is put back (the fix-round list is in
the comments).
    py -I tools/q/qbody_selftest.py
"""
import hashlib
import os
import sqlite3
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qbody  # noqa: E402
import qfresh  # noqa: E402
import qtools  # noqa: E402

FAILS = []


def check(name, cond, detail=''):
    print(('ok   ' if cond else 'FAIL ') + name + (f'  {detail}' if detail and not cond else ''))
    if not cond:
        FAILS.append(name)


CPP = '''#include <x>
namespace n {
struct S { int a; };
void proto(int x);
int f(int a) {
  if (a) { return 1; }
  const char* s = "}{ not a brace";   // } also not
  const char* t = "{";
  return 0;
}
class C {
 public:
  void m() { }
};
static void g() {
}
}  // namespace n
'''
# f = 5-10, m = 13-13, g = 15-16

ROW = ('u1', 'a/t.cpp', 5, 10)


def mkdb(path_text, sha=None, size=None):
    con = sqlite3.connect(':memory:')
    con.execute('CREATE TABLE funcs(usr TEXT PRIMARY KEY, file TEXT, line INTEGER, endline INTEGER)')
    con.execute('CREATE TABLE syms(usr TEXT, qname TEXT, name TEXT, kind INTEGER, file TEXT, line INTEGER, isdef INTEGER)')
    con.execute('CREATE TABLE files(path TEXT, sha TEXT, size INTEGER)')
    con.execute("INSERT INTO files VALUES('a/t.cpp', ?, ?)", (sha, size))
    con.execute("INSERT INTO funcs VALUES('u1','a/t.cpp',5,10)")
    con.execute("INSERT INTO syms VALUES('u1','n::f','f',8,'a/t.cpp',5,1)")
    con.execute("INSERT INTO syms VALUES('u2','n::S','S',2,'a/t.cpp',3,1)")
    con.execute("INSERT INTO syms VALUES('u3','n::S::a','a',6,'a/t.cpp',3,1)")
    return con


def write(td, rel, text):
    p = os.path.join(td, *rel.split('/'))
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    return p


def sha(text):
    return hashlib.sha1(text.encode()).hexdigest()


FS = lambda c, nm: [dict(usr='u1', qname='n::f', kind=8)]   # noqa: E731


def main():
    ls = CPP.split('\n')
    got = {n: (a, b) for n, a, b in qbody.scan_funcs(ls)}
    check('scan finds f with its true span', got.get('f') == (5, 10), str(got))
    check('scan finds g (after the string / comment braces)', got.get('g') == (15, 16), str(got))
    check('scan finds the in-class method', got.get('m') == (13, 13), str(got))
    check('scan does not count the prototype', 'proto' not in got, str(got))
    check('scan does not count if (..) { inside a body', 'if' not in got, str(got))
    # twin: with STRING stripping removed the `"{"` line (net +1) would swallow g; the real brace inside a string, not a comment
    check('planted: a { inside a string does not open a block (f ends at 10, g still found)', got.get('f', (0, 0))[1] == 10 and 'g' in got)

    # L2: scan shapes the first version missed
    L2 = '''namespace n
{
extern "C"
{
/* block comment { not a brace */
struct Check { int a; };
void imp(const Check& check = {}, int k = 3) {
  (void)check;
}
#if 0
void dead() {
#else
void live() {
#endif
}
}
}
class K {
 public:
  K(int a) : a_{a}, b_(2) {
  }
  int a_;
  int b_;
  bool operator==(const K& o) const { return a_ == o.a_; }
};
REXCVAR_DEFINE_STRING(x, "{slot,", "d",
    "text");
void after() {
}
'''
    g2 = {n: (a, b) for n, a, b in qbody.scan_funcs(L2.split('\n'))}
    check('scan: namespace / extern "C" with the brace on the next line, block comment brace', g2.get('imp') == (7, 9), str(g2))
    check('scan: default argument `= {}` is not the body brace', 'check' not in g2 and g2.get('imp') == (7, 9), str(g2))
    check('scan: #if 0 / #else with duplicate braces counts one branch', g2.get('live') == (13, 15) and 'dead' not in g2, str(g2))
    check('scan: constructor with a brace initialiser, and operator==', g2.get('K') == (20, 21) and 'operator==' in g2, str(g2))
    check('scan: a macro with a string holding { is not a function with a huge span; code after it is found', 'after' in g2 and 'REXCVAR_DEFINE_STRING' not in g2, str(g2))

    with tempfile.TemporaryDirectory() as td:
        write(td, 'a/t.cpp', CPP)
        con = mkdb(None, sha(CPP), len(CPP.encode()))
        # fresh file (sha + size match): plain [index]
        e = qbody.cmd_enclosing(con, td, 'a/t.cpp:6')
        check('enclosing: line inside f, file fresh -> [index]', 'in n::f' in e[0] and '5-10' in e[0] and e[0].endswith('[index]'), e[0])
        e = qbody.cmd_enclosing(con, td, 't.cpp:3')
        check('planted: a line outside every function is NOT attributed to one', 'not inside a function' in e[0] and 'in n::f' not in e[0], e[0])
        o = qbody.cmd_outline(con, td, 'a/t.cpp')
        check('outline: function span + struct with field, no bodies', any('5-10' in x and 'n::f' in x for x in o) and any('struct n::S' in x and 'a@3' in x for x in o)
              and not any('return' in x for x in o), '\n'.join(o))
        b = qbody.cmd_body(con, td, 'f', 400, FS, None)
        check('body: exactly the function lines 5-10, label [index]', b[0].endswith('[index]') and b[1].startswith('5\t') and b[-1].startswith('10\t') and len(b) == 7, '\n'.join(b))
        # enclosing takes the innermost; twin for `outermost`
        con.execute("INSERT INTO funcs VALUES('u4','a/t.cpp',6,6)")
        con.execute("INSERT INTO syms VALUES('u4','n::f::lam','lam',8,'a/t.cpp',6,1)")
        e = qbody.cmd_enclosing(con, td, 'a/t.cpp:6')
        check('planted: enclosing names the INNERMOST function (nested lambda), not the outermost', 'in n::f::lam' in e[0] and 'also inside n::f' in '\n'.join(e), '\n'.join(e))
        con.execute("DELETE FROM funcs WHERE usr='u4'")

        # a file the index does not know
        write(td, 'a/new.h', CPP)
        funcs, src, _ = qbody.file_funcs(con, td, 'a/new.h')
        check('unindexed file: text scan, labelled', src.startswith('text scan') and any(n == 'f' for n, _, _, _ in funcs), src)
        b = qbody.cmd_body(con, td, 'g', 400, lambda c, nm: [], {'new': ['a/new.h'], 'changed': []})
        check('body of a symbol the branch added: scanned and labelled', 'text scan' in b[0] and b[1].startswith('15\t'), '\n'.join(b))

        # M3: distinct absence messages
        try:
            qbody.cmd_body(con, td, 'nope', 400, lambda c, nm: [], {'new': ['a/new.h'], 'changed': []})
            check('planted: a missing function raises', False)
        except qbody.BodyError as ex:
            check('M3: the scan found nothing (N files scanned) is not "no such function"', 'text scan of 1 edited' in str(ex) and 'no such function' not in str(ex), str(ex))
        try:
            qbody.cmd_body(con, td, 'nope', 400, lambda c, nm: [], {'new': [], 'changed': []})
            check('planted: a missing function raises', False)
        except qbody.BodyError as ex:
            check('M3: nothing scanned says no such function', 'no such function' in str(ex), str(ex))

        # ---- H1: the branch ADDED h between f and g (rows are the remapped index rows: f 5-10, g shifted +5 to 20-21)
        EDIT = CPP.replace('static void g() {', 'int h(int z) {\n  int y = z;\n  return y + 2;\n}\nstatic void g() {')
        write(td, 'a/t.cpp', EDIT)
        con2 = mkdb(None, sha(CPP), len(CPP.encode()))
        con2.execute("INSERT INTO funcs VALUES('u5','a/t.cpp',20,21)")
        con2.execute("INSERT INTO syms VALUES('u5','n::g','g',8,'a/t.cpp',20,1)")
        # real positions in EDIT: f 5-10, m 13, h 15-18, g 19-20
        info = {'changed': ['a/t.cpp'], 'mapped': ['a/t.cpp'], 'stale': [], 'new': []}
        o = '\n'.join(qbody.cmd_outline(con2, td, 'a/t.cpp', 400, info))
        check('H1: outline of a changed file lists the function the branch added, marked', ' n::f' in o and ' h' in o and 'added by this branch' in o and '[index]' not in o.split('\n')[0], o)
        e = qbody.cmd_enclosing(con2, td, 'a/t.cpp:16', info)
        check('H1: enclosing inside the added function names it (not "not inside a function")', 'in h' in e[0] and '[index]' not in e[0], e[0])

        # ---- H2: a hunk rewrote f's closing line and added h: the remapped end of f points at h's last line
        H2 = 'namespace n {\n\nint f(int a) {\n  return a;\n}\nint h(int z) {\n  int y = z;\n  return y;\n}\n}\n'
        write(td, 'a/t.cpp', H2)
        con3 = mkdb(None, sha(CPP), len(CPP.encode()))
        con3.execute("UPDATE funcs SET line=3, endline=9 WHERE usr='u1'")        # what rle gives: the hunk's LAST new line
        b = '\n'.join(qbody.cmd_body(con3, td, 'f', 400, FS, {'changed': ['a/t.cpp'], 'mapped': ['a/t.cpp'], 'stale': [], 'new': []}))
        check('H2: body f stops at f\'s real end (5) and does not print h', '5\t}' in b and 'int h' not in b and '[index]' not in b.split('\n')[0], b)
        e = qbody.cmd_enclosing(con3, td, 'a/t.cpp:7', {'changed': ['a/t.cpp'], 'mapped': ['a/t.cpp'], 'stale': [], 'new': []})
        check('H2: enclosing a line inside h names h, not f', 'in h' in e[0] and 'in n::f' not in e[0], e[0])

        # ---- M1: stale file (not remapped at all): index lines are the old ones, text shifted by 2
        SH = '// a\n// b\n' + CPP
        write(td, 'a/t.cpp', SH)
        b = qbody.cmd_body(con3, td, 'f', 400, FS, {'changed': ['a/t.cpp'], 'mapped': [], 'stale': ['a/t.cpp'], 'new': []}) if False else None
        con4 = mkdb(None, sha(CPP), len(CPP.encode()))
        b = qbody.cmd_body(con4, td, 'f', 400, FS, {'changed': ['a/t.cpp'], 'mapped': [], 'stale': ['a/t.cpp'], 'new': []})
        check('M1: a stale file prints the real function (7-12) under a label that is not plain [index]', b[1].startswith('7\t') and 'int f(' in b[1] and b[-1].startswith('12\t')
              and not b[0].split('  ')[-1] == '[index]' and 'NOT remapped' in b[0], '\n'.join(b))
        # main checkout (no info): the file differs from the files row (sha) -> not [index]
        b = qbody.cmd_body(con4, td, 'f', 400, FS, None)
        check('M1: main checkout, file differs from the index build (sha) -> never a plain [index]', b[1].startswith('7\t') and '[index]' not in b[0], '\n'.join(b))
        # the same file with the row's sha right: plain [index]
        con5 = mkdb(None, sha(SH), len(SH.encode()))
        con5.execute("UPDATE funcs SET line=7, endline=12 WHERE usr='u1'")
        b = qbody.cmd_body(con5, td, 'f', 400, FS, None)
        check('M1 control: sha + size match -> [index]', b[0].endswith('[index]') and b[1].startswith('7\t'), '\n'.join(b))

        # sha cache -----------------------------------------------------------------------------------------------
        p = os.path.join(td, 'x.cpp')
        with open(p, 'w') as f:
            f.write('aaaa')
        old_ns = 5_000_000_000
        st = os.stat(p)
        os.utime(p, ns=(st.st_atime_ns, st.st_mtime_ns - old_ns))      # an old file: its cache entry becomes trusted
        qfresh.sha_cache_open(td)
        h1 = qfresh.sha_of(p)
        qfresh.sha_cache_save()
        check('sha cache written', os.path.isfile(os.path.join(td, 'out', 'harness', 'qfresh_sha.json')))
        qfresh.sha_cache_open(td)
        rows = qfresh._SHA_CACHE['rows']
        rows[p][2] = 'SENTINEL'
        check('a trusted entry is returned without re-reading', qfresh.sha_of(p) == 'SENTINEL')
        # M2: a same-size write that leaves the SAME mtime (same clock tick / a copy that kept the old time) after the entry was cached
        q = os.path.join(td, 'y.cpp')
        with open(q, 'w') as f:
            f.write('aaaa')
        qfresh.sha_cache_open(td)
        ha = qfresh.sha_of(q)
        st = os.stat(q)
        with open(q, 'w') as f:
            f.write('bbbb')
        os.utime(q, ns=(st.st_atime_ns, st.st_mtime_ns))               # exactly the first write's mtime
        hb = qfresh.sha_of(q)
        check('planted M2: a same-tick same-size rewrite is re-hashed (racy entry never trusted)', ha != hb)
        with open(p, 'w') as f:
            f.write('bbbb')
        st = os.stat(p)
        os.utime(p, ns=(st.st_atime_ns, st.st_mtime_ns + 5_000_000_000))
        qfresh.sha_cache_open(td)
        h2 = qfresh.sha_of(p)
        check('planted: same-size edit with a new mtime is re-hashed', h1 != h2)
        # L4: a failed save leaves no .tmp behind
        qfresh.sha_cache_open(td)
        qfresh.sha_of(os.path.join(td, 'a', 'new.h'))
        real = os.replace

        def boom(a, b):
            raise PermissionError('held open')
        os.replace = boom
        try:
            qfresh.sha_cache_save()
        finally:
            os.replace = real
        left = [x for x in os.listdir(os.path.join(td, 'out', 'harness')) if x.endswith('.tmp')]
        check('L4: a lost cache save leaves no .tmp file', not left, str(left))
        qfresh._SHA_CACHE['path'] = None

        # q py body
        write(td, 'tools/m.py', 'def one():\n    return 1\n\n\ndef two(a):\n    x = a\n    return x\n')
        write(td, 'tools/m.ps1', 'function Do-It {\n  param($a)\n  if ($a) { 1 }\n}\n$z = 1\n')
        idx = qtools.load(td)

        class A:
            limit = 30
        r = qtools.cmd_body(idx, 'two', A, td)
        check('py body: the function lines only', r and r['bodies'][0]['line'] == 5 and r['bodies'][0]['endline'] == 7 and len(r['bodies'][0]['lines']) == 3, str(r))
        r = qtools.cmd_body(idx, 'Do-It', A, td)
        check('ps1 body: brace-matched end', r and r['bodies'][0]['endline'] == 4, str(r))
        check('planted: a variable / unknown name has no body', qtools.cmd_body(idx, 'z', A, td) is None and qtools.cmd_body(idx, 'nothere', A, td) is None)

    # L5: a failed git call is not remembered
    try:
        import qours
        calls = []
        real_unc = qours._main_checkout_uncached

        def flaky(root):
            calls.append(root)
            if len(calls) == 1:
                raise OSError('transient')
            return 'MAIN'
        qours._main_checkout_uncached = flaky
        qours._MAIN_CACHE.pop('zz_root', None)
        r1 = qours._main_checkout('zz_root')
        r2 = qours._main_checkout('zz_root')
        qours._main_checkout_uncached = real_unc
        qours._MAIN_CACHE.pop('zz_root', None)
        check('L5: a transient failure is not memoised (the second call asks again)', r1 is None and r2 == 'MAIN', f'{r1} {r2}')
    except ImportError as ex:
        check('L5: qours importable', False, str(ex))

    # q note
    import qnote
    NOTE = ('# T\n\nintro\n\n## Summary of evidence\n\nSUMMARYTOKEN\n\n## Verdict\n\nthe answer\n```\n## not a heading\n```\n\n## Evidence\n\nrows\n\n### Sub sites\n\nSUBSITETOKEN\n\n'
            '## Sites the build will touch\n\n1. a.cpp:1\n')
    nl = NOTE.split('\n')
    hs = qnote.headings(nl)
    check('note: a ## inside a fence is not a heading', not any(h[1] == 'not a heading' for h in hs), str(hs))
    with tempfile.TemporaryDirectory() as td, tempfile.TemporaryDirectory() as md:
        write(td, 'docs/research/zz_note.md', NOTE)
        old = qnote.ROOT, qnote.MAIN
        qnote.ROOT, qnote.MAIN = td, td
        try:
            v = '\n'.join(qnote.answer('zz_note', 'verdict'))
            check('note --verdict: the Verdict section (outranks an earlier "Summary of ...")', 'the answer' in v and 'SUMMARYTOKEN' not in v and 'rows' not in v, v)
            s = '\n'.join(qnote.answer('zz_note', 'sites'))
            check('note --sites: the sites list only (not the "Sub sites" heading)', 'a.cpp:1' in s and 'the answer' not in s and 'SUBSITETOKEN' not in s, s)
            e = '\n'.join(qnote.answer('zz_note', 'section', 'evidence'))
            check('note --section keeps the ### child, stops at the next ##', 'rows' in e and 'SUBSITETOKEN' in e and 'a.cpp:1' not in e, e)
            try:
                qnote.answer('zz_note', 'section', 'no such heading')
                check('planted: an unknown heading raises (never an empty PASS)', False)
            except qnote.NoteError:
                check('planted: an unknown heading raises (never an empty PASS)', True)
            write(td, 'docs/research/survey.md', '# S\n\n## Findings\n\nx\n')
            try:
                qnote.answer('survey', 'sites')
                check('planted: a survey note without a sites section is refused', False)
            except qnote.NoteError:
                check('planted: a survey note without a sites section is refused', True)
            write(td, 'docs/research/many.md', '# M\n' + ''.join(f'\n## Part {i}\n\nline {i}\n' for i in range(9)))
            m = '\n'.join(qnote.answer('many', 'section', 'part'))
            check('L1: --section says how many matches it did not show', '3 more sections match and are NOT shown' in m, m)
        finally:
            qnote.ROOT, qnote.MAIN = old
        # M4: a worktree copy older than main's: main's text is read and the label says so
        write(td, 'docs/research/zz_two.md', '# T\n\n## Verdict\n\nOLDTEXT\n')
        write(md, 'docs/research/zz_two.md', '# T\n\n## Verdict\n\nNEWTEXT\n')
        qnote.ROOT, qnote.MAIN = td, md
        try:
            m = '\n'.join(qnote.answer('zz_two', 'verdict'))
            check('M4: both copies exist and differ -> main\'s text, labelled', 'NEWTEXT' in m and 'OLDTEXT' not in m and 'main checkout' in m and 'worktree copy differs' in m, m)
            write(td, 'docs/research/zz_only.md', '# T\n\n## Verdict\n\nWTONLY\n')
            m = '\n'.join(qnote.answer('zz_only', 'verdict'))
            check('M4: only the worktree has it -> its text, label names the worktree', 'WTONLY' in m and 'this worktree checkout' in m, m)
        finally:
            qnote.ROOT, qnote.MAIN = old
    print('FAILED: ' + ', '.join(FAILS) if FAILS else 'all ok')
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
