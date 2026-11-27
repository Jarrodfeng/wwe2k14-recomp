#!/usr/bin/env python3
"""Selftest for `q ours` as TEXT search (S110 QTEXT, tools/q/qsym.py). Every case goes through the REAL CLI (`py -I tools/q/q.py ours ... --json` as a subprocess)
against a fixture git repo in a TEMP folder. 0 game runs, 0 compiles.

    py -I tools/q/qsym_selftest.py [--q PATH_TO_q.py] [--keep]

PLANTED TWINS (QOURS_plan.md section 2, the mechanisms by which the compiler-accurate index was wrong or empty). Each is one fixture and one count:
  T1 a tools-test global `g_x` beside `zq::g_x`: both are answered (the exact-qname global used to hide the namespaced one)
  T2 a Python `hold` beside C++ `Hold`: `callers hold` answers the python def and NO C++ symbol (case-blind tails)
  T3 function-template calls: `ReadMip0<int>(2)`, `ReadMip0(1)` and one inside a never-instantiated template: all found
  T4 `s_ = std::string(...)` is a write of s_ (a class-type assignment used to count as a read)
  T5 `namespace dirty_ns` and `using Alias = int;`: `def` finds both
  T6 a header no unit includes: its callers are found
  T7 `A::Pick` vs `B::Pick`: the scope word narrows (the call site that never names A is counted, not shown)
and the rest of the contract: enclosing function / comment / string tags, braces in strings and comments, JSON shape, the verbs that went (exit 2 + a message),
`update` exits 0 and says the index is gone, the answer is the same in a git worktree, each call is quick.
With --q pointing at a tree that still has the index (build_ours.py beside it) the fixture is indexed first: T1-T6 must FAIL there (the twins).
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))

FIX = {
    'src/zq/g.h': '''#pragma once
#include <atomic>
#include <string>
namespace zq {
inline std::atomic<int> g_x{0};
inline std::string s_;
namespace dirty_ns { inline int v = 1; }
using Alias = int;
template <class T> T ReadMip0(T v) { return v; }
class Hold { public: static int Run(int a) { return a; } };
}  // namespace zq
''',
    'src/zq/use.cpp': '''#include "g.h"
namespace zq {
int UseIt() {
  g_x.store(3);
  s_ = std::string("a") + "b";
  int a = ReadMip0(1);
  int b = ReadMip0<int>(2);
  return a + b + Hold::Run(g_x.load());
}
template <class T> int Wrap(T v) { return ReadMip0<T>(v); }
}  // namespace zq
''',
    'tools/zq/zq_test.cpp': '''#include "../../src/zq/g.h"
int g_x = 5;   // a tools-test global named like zq::g_x
int main() { g_x = 6; return g_x; }
''',
    'src/zq/orphan.h': '''#pragma once
inline int ReadGuestBytes(int a) { return a; }
inline int Orph() { return ReadGuestBytes(1) + ReadGuestBytes(2); }
''',
    'tools/zq/t.py': 'def hold(x):\n    return x\n\n\ndef zq_py_only(x):\n    return x\n\n\nprint(hold(1), zq_py_only(2))\n',
    'src/zq/note.h': '#pragma once\n// tools/zq/t.py zq_py_only() writes the table this header reads\n',
    'src/zq/a.h': 'namespace A { inline int Pick(int x) { return x; } }\n',
    'src/zq/b.h': 'namespace B { inline int Pick(int x) { return x + 1; } }\n',
    'src/zq/a_use.cpp': '#include "a.h"\nint UseA() { return A::Pick(1); }\n',
    'src/zq/b_use.cpp': '#include "b.h"\nint UseB() { return B::Pick(2); }\n',
    'src/zq/brace.cpp': '''namespace zq {
int Tricky() {
  const char* s = "}}}{";   // } comment {{
  /* } */
  return s[0];
}
int After() { return 2; }
}  // namespace zq
''',
    # S111 SPLIT-WIRE review: a split part (tools/split/read_unit.py) holds members of a class its parent opens; the part never names the class
    'src/zq/big.h': '#pragma once\nnamespace zq {\nclass Big {\n public:\n#include "big/members.inc"  // split: members\n};\n}  // namespace zq\n',
    'src/zq/big/members.inc': '  int Member(int a) { return a + 1; }\n  int Other() { return Member(2); }\n',
    'src/zq/big_use.cpp': '#include "big.h"\nint UseBig() { zq::Big b; return b.Member(1); }\n',
}


def write_fixture(root):
    for rel, txt in FIX.items():
        p = os.path.join(root, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, 'w', encoding='utf-8', newline='\n') as f:
            f.write(txt)
    units = [os.path.join(root, r) for r in FIX if r.endswith('.cpp')]
    for name in ('cc_on', 'cc_off'):
        d = os.path.join(root, 'out', 'index', name)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, 'compile_commands.json'), 'w') as f:
            json.dump([{'directory': root, 'file': u, 'arguments': ['clang++', '-std=c++20', f'-I{root}/src', '-c', u]} for u in units], f)
    with open(os.path.join(root, '.gitignore'), 'w') as f:
        f.write('out/\n')
    for a in (['init', '-q', '-b', 'main'], ['add', '-A'], ['commit', '-q', '-m', 'fixture']):
        subprocess.run(['git', '-C', root, '-c', 'user.name=t', '-c', 'user.email=t@t', '-c', 'core.autocrlf=false'] + a, capture_output=True, check=True)


FAILS = []


def check(cond, what, detail=''):
    print(('PASS  ' if cond else 'FAIL  ') + what)
    if not cond:
        FAILS.append(what)
        if detail:
            print('      ' + str(detail).strip().replace('\n', '\n      ')[:1200])


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--q', default=os.path.join(HERE, 'q.py'))
    ap.add_argument('--keep', action='store_true')
    a = ap.parse_args(argv)
    Q = os.path.abspath(a.q)
    old = os.path.exists(os.path.join(os.path.dirname(Q), 'build_ours.py'))
    env = dict(os.environ, WWE_Q_TIMEOUT='300', PYTHONIOENCODING='utf-8', WWE_Q_OURS_NO_AUTOUPDATE='1')
    root = tempfile.mkdtemp(prefix='qsym_selftest_')
    wt = root + '_wt'

    def q(*args, cwd=None, timeout=300):
        t0 = time.time()
        r = subprocess.run([sys.executable, '-I', Q] + list(args), capture_output=True, text=True, encoding='utf-8', errors='replace', env=env, timeout=timeout, cwd=cwd)
        return r.returncode, r.stdout, r.stderr, time.time() - t0

    def ours(*args, rt=None):
        rc, so, se, dt = q('ours', *args, '--json', '--all', '--root', rt or root)
        try:
            js = json.loads(so[so.index('{'):])
        except (ValueError, json.JSONDecodeError):
            js = None
        return rc, js, so + se, dt

    def files_lines(js):
        out = {}
        for m in (js or {}).get('matches', []):
            for s in m.get('sites', []):
                out.setdefault(s['file'], set()).update(s['lines'])
        return out

    try:
        write_fixture(root)
        if old:
            r = subprocess.run([sys.executable, '-I', os.path.join(os.path.dirname(Q), 'build_ours.py'), '--root', root, '-j', '2'], capture_output=True, text=True)
            print('(old tree: index built, rc %d)' % r.returncode)
        # T1
        rc, js, raw, dt = ours('refs', 'g_x')
        fl = files_lines(js)
        check('src/zq/use.cpp' in fl and 'tools/zq/zq_test.cpp' in fl, 'T1 refs g_x: the namespaced zq::g_x use AND the tools-test global are both answered', raw)
        # T2
        rc, so, se, dt = q('ours', 'callers', 'hold', '--root', root)
        check(rc == 0 and 'tools/zq/t.py' in so and 'Hold' not in so + se and 'g.h' not in so + se, 'T2 callers hold (python): the python def answers, no C++ Hold', so + se)
        # T3
        rc, js, raw, dt = ours('refs', 'ReadMip0')
        fl = files_lines(js)
        check(fl.get('src/zq/use.cpp') == {6, 7, 10}, 'T3 refs ReadMip0: the deduced call (6), the template-argument call (7) and the call inside a never-instantiated template (10)', raw)
        # T4
        rc, js, raw, dt = ours('writes', 's_')
        fl = files_lines(js)
        check(fl.get('src/zq/use.cpp') == {5}, 'T4 writes s_: `s_ = std::string(...)` is a write', raw)
        # T5
        r1 = ours('def', 'dirty_ns')
        r2 = ours('def', 'Alias')
        check(r1[0] == 0 and r2[0] == 0 and any(m['file'] == 'src/zq/g.h' for m in (r1[1] or {}).get('matches', [])) and any(m['file'] == 'src/zq/g.h' for m in (r2[1] or {}).get('matches', [])),
              'T5 def dirty_ns / def Alias: the namespace and the alias are found', r1[2] + r2[2])
        # T6
        rc, js, raw, dt = ours('callers', 'ReadGuestBytes')
        fl = files_lines(js)
        check(fl.get('src/zq/orphan.h') == {3}, 'T6 callers ReadGuestBytes: the calls in a header no unit includes are found', raw)
        # T7
        rc, js, raw, dt = ours('callers', 'A::Pick')
        fl = files_lines(js)
        check(set(fl) == {'src/zq/a_use.cpp'}, 'T7 callers A::Pick: the call that never names A (b_use.cpp) is not shown', raw)
        rc, js, raw, dt = ours('callers', 'Pick')
        check(set(files_lines(js)) == {'src/zq/a_use.cpp', 'src/zq/b_use.cpp'}, 'T7b callers Pick (no scope): both uses', raw)
        if not old:
            # T8 (QTEXT review): a python name that C++ only MENTIONS in a comment: the comment hit must not stop the tools index from answering
            rc, so, se, dt = q('ours', 'callers', 'zq_py_only', '--root', root)
            check(rc == 0 and 'tools/zq/t.py' in so, 'T8 callers zq_py_only (python; a C++ comment names it): the python call site answers, not "0 sites"', so + se)
            rc, so, se, dt = q('ours', 'def', 'zq_py_only', '--root', root)
            check(rc == 0 and 'tools/zq/t.py' in so, 'T8b def zq_py_only: the python def answers, not "no definition-shaped line"', so + se)
        if old:
            print('old tree: the rest of the contract (text tags, verbs that went, worktree) is the new form only')
        else:
            # enclosing / tags / braces
            rc, so, se, dt = q('ours', 'enclosing', 'src/zq/use.cpp:6', '--root', root)
            check(rc == 0 and 'zq::UseIt' in so, 'enclosing use.cpp:6 = zq::UseIt', so + se)
            rc, so, se, dt = q('ours', 'enclosing', 'src/zq/brace.cpp:7', '--root', root)
            check(rc == 0 and 'zq::After' in so, 'braces in a string and in comments do not confuse the scan: line 7 is zq::After', so + se)
            rc, so, se, dt = q('ours', 'outline', 'src/zq/brace.cpp', '--root', root)
            check(rc == 0 and 'zq::Tricky' in so and 'zq::After' in so and '2-6' in so and '7-7' in so, 'outline brace.cpp: Tricky 2-6 and After 7-7', so + se)
            rc, js, raw, dt = ours('refs', 'Tricky')
            check(rc == 2 or js is not None, 'refs of a name with only its definition: answered, not a crash', raw)
            rc, so, se, dt = q('ours', 'body', 'After', '--root', root)
            check(rc == 0 and 'int After() { return 2; }' in so, 'body After: the function text', so + se)
            rc, js, raw, dt = ours('refs', 'g_x', '--limit', '1')
            ms = (js or {}).get('matches', [{}])
            check(ms and all(k in ms[0] for k in ('qname', 'sites', 'n')) and all({'func', 'file', 'lines', 'kinds'} <= set(s) for s in ms[0]['sites']),
                  'JSON shape: matches[].{qname, sites[].{func, file, lines, kinds}, n}', raw)
            func = {s['func'] for m in ms for s in m['sites'] if s['file'] == 'src/zq/use.cpp'}
            check(func == {'zq::UseIt'}, 'the enclosing function of a site is its qualified name', str(func))
            rc, js, raw, dt = ours('refs', 'zq::Hold')
            check(rc == 0 and 'src/zq/use.cpp' in files_lines(js), 'a qualified name: Scope::name keeps the use that names the scope', raw)
            # T9 a split part: its members read in the scope its parent opens at the `// split:` include line, and keep their real file:line
            rc, so, se, dt = q('ours', 'fn', 'Big::Member', '--root', root)
            check(rc == 0 and 'zq::Big::Member' in so and 'src/zq/big/members.inc:1' in so, 'T9 fn Big::Member: found in the part, qualified by the parent\'s class', so + se)
            rc, so, se, dt = q('ours', 'outline', 'src/zq/big/members.inc', '--root', root)
            check(rc == 0 and 'zq::Big::Member' in so and 'zq::Big::Other' in so, 'T9b outline of the part: zq::Big::Member, zq::Big::Other', so + se)
            rc, so, se, dt = q('ours', 'enclosing', 'src/zq/big/members.inc:2', '--root', root)
            check(rc == 0 and 'zq::Big::Other' in so, 'T9c enclosing members.inc:2 = zq::Big::Other', so + se)
            rc, so, se, dt = q('ours', 'outline', 'src/zq/big.h', '--root', root)
            check(rc == 0 and 'src/zq/big/members.inc' in so, 'T9d outline of the parent names its part', so + se)
            rc, js, raw, dt = ours('callers', 'Big::Member')
            fl = files_lines(js)
            check(fl.get('src/zq/big/members.inc') == {2} and 'src/zq/big_use.cpp' in fl, 'T9e callers Big::Member: the call inside the part (which never names Big) and the one in big_use.cpp', raw)
            # the verbs that went
            for verb in ('rows', 'diff'):
                rc, so, se, dt = q('ours', verb, '--root', root)
                check(rc == 2 and 'gone' in se, f'{verb}: gone, exit 2, the message says so', so + se)
            rc, so, se, dt = q('ours', 'refs', 'g_x', '--branch', 'main', '--root', root)
            check(rc == 2 and 'gone' in se, 'refs --branch REF: gone, exit 2', so + se)
            rc, so, se, dt = q('ours', 'update')
            check(rc == 0 and 'no index' in so and len(so.strip().splitlines()) == 1, 'update: one line saying the index is gone, exit 0', so + se)
            rc, so, se, dt = q('ours', 'refs', 'zq_nothing_at_all', '--root', root)
            check(rc == 2, 'a name with no text hit anywhere: exit 2 (never an empty answer that reads as success)', so + se)
            rc, so, se, dt = q('ours', 'callees', 'UseIt', '--root', root)
            check(rc == 0 and 'ReadMip0' in so and 'Run' in so, 'callees UseIt: the names it calls, by text', so + se)
            rc, so, se, dt = q('ours', 'covers', 'src/zq/g.h', '--root', root)
            check(rc == 0 and 'src/zq/use.cpp' in so and 'tools/zq/zq_test.cpp' in so, 'covers g.h: the units that include it', so + se)
            # a git worktree gives the same answer
            subprocess.run(['git', '-C', root, 'worktree', 'add', '-q', wt, '-b', 'wtb'], capture_output=True, check=True)
            c1 = q('ours', 'callers', 'ReadGuestBytes', '--root', root)
            c2 = q('ours', 'callers', 'ReadGuestBytes', '--root', wt)
            check(c1[0] == 0 and c1[1] == c2[1], 'the same answer in the main checkout and in a git worktree', c1[1] + '|' + c2[1])
            # quick
            slow = []
            for args in (('refs', 'g_x'), ('callers', 'ReadMip0'), ('def', 'Hold'), ('outline', 'src/zq/use.cpp')):
                rc, so, se, dt = q('ours', *args, '--root', root)
                if dt > 3.0:
                    slow.append((args, dt))
            check(not slow, 'each call answers in a few seconds (a fixture: ~0.3 s)', str(slow))
    finally:
        if not a.keep:
            shutil.rmtree(wt, ignore_errors=True)      # the fixture repo goes with its worktree record
            shutil.rmtree(root, ignore_errors=True)
        else:
            print(f'kept {root}')
    if FAILS:
        print(f'qsym_selftest: FAIL ({len(FAILS)} checks)')
        return 1
    print('qsym_selftest: PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())
