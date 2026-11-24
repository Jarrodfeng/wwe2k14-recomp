"""S110 PLANTONCE: selftest of tools/harness/plant_once.py on a synthetic header + unit (tools/harness/fixtures/plant_once). No game runs.

    py -I tools/harness/plant_once_selftest.py

Checks the eight results of the design note (docs/research/s110/PLANTONCE.md, site 4) plus the brace walk, through the real run_once with a
real recompile callback (one plant, whole header compiled alone, as the scripts do). Exit 0 = every expectation held.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import plant_once as po  # noqa: E402

FX = os.path.join(HERE, 'fixtures', 'plant_once')
FLAGS = ['-std=c++20', '-Wall']

PLANTS = [
    po.Plant('clamp_expr', 'fx.h', r'v > kLimit\)', 'v > kLimit + 5)'),
    po.Plant('clamp_below_deleted', 'fx.h', r'  if \(v < 0\) return 0;(\r?\n)', r'\1'),
    po.Plant('sum_init_one', 'fx.h', r'int s = 0;', 'int s = 1;'),
    po.Plant('sum_init_two', 'fx.h', r'int s = 0;', 'int s = 2;'),
    po.Plant('limit_eleven', 'fx.h', r'kLimit = 10;', 'kLimit = 11;'),
    po.Plant('twice_off', 'fx.h', r'return x \* 2;', 'return x * 2 + 1;'),
    po.Plant('add_minus', 'fx.h', r'return a \+ b;', 'return a - b;'),
    po.Plant('clamp_broken', 'fx.h', r'return kLimit;', 'return kLimit +;'),
    po.Plant('unchecked_off', 'fx.h', r'return v \+ 0;', 'return v + 1;'),
    po.Plant('box_ctor', 'fx.h', r'total = w;', 'total = 5;'),
    po.Plant('literal_rep', 'fx.h', r'int s = 0;', r'int s = 3; // \1', literal=True),
    po.Plant('gone', 'fx.h', r'no such text anywhere', 'x'),
]

# (S110 PLANTONCE review) fx2.h: plants whose run-time result in the combined exe is not the per-plant compile's; none may be reported 'run-time'.
# The fx2 unit's verdict is the exit code alone (as seats / seat_card), so a crash would read as caught.
PLANTS2 = [
    po.Plant('param_control', 'fx2.h', r'return w;', 'return w + 1;'),
    # the exit code of an access violation, without one (a real crash would raise the Windows error dialog)
    po.Plant('crash_exit', 'fx2.h', r'if \(v < 0\) return 0;', 'if (v < 0) return 0; if (v == 5) std::_Exit(-1073741819);'),
    # a parameter redeclared in the body's outermost block: an error alone, a legal shadow inside the combined exe's if-branch
    po.Plant('param_redeclared', 'fx2.h', r'int w = v;', 'int v = 9; int w = v;'),
    # braces that close the body early and open a new block: an error alone (an `if` at namespace scope), compiles in the if-chain
    po.Plant('brace_split', 'fx2.h', r'if \(v > 100\) return 100;', 'if (v > 100) return 101; } if (v == 77) { return 0;'),
    # an uninitialised read: the per-plant build and the combined exe read different stack garbage
    po.Plant('uninit_read', 'fx2.h', r'  r = v;(\r?\n)', r'\1'),
    # consteval body that is no constant expression: an error alone, a run-time call once the keyword is gone
    po.Plant('consteval_runtime', 'fx2.h', r'return 7;', 'return std::atoi("8");'),
]


def find_clang():
    return po.find_clang()


def verdict(rc, out):
    return rc != 0 and any(l.startswith('FAIL ') for l in out.splitlines()) and 'FAIL fx_unit' in out


def main():
    fails = []

    def expect(cond, what):
        if not cond:
            fails.append(what)

    # the brace walk
    src = ('namespace n { struct S { int a{3}; S() : a{1} { a = 2; } int F() const { return a; } };\n'
           'enum class E : int { A = 1, B }; template <class T> T G(T x) { return x; }\n'
           'inline const int k[] = {1, 2}; inline auto L = [](int y) { return y; };\n'
           'inline int H(int v) { if (v) { return 1; } return 0; } /* int Z() { } */ inline const char* s = "{ not a body }"; }\n')
    bodies = [src[o:c + 1].split('{')[1].strip()[:12] for o, c in po.function_bodies(src)]
    expect(len(bodies) == 4, 'function_bodies found %d bodies, wanted 4 (ctor, F, G, H): %r' % (len(bodies), bodies))
    # (review) a prefixed char literal L'a' is not a digit separator: the '{' after it must not be blanked (the body would end at the if's '}')
    wsrc = "inline int W(wchar_t c) {\n  if (c == L'a') {\n    return 1;\n  }\n  return u8'b';\n}\ninline int V() { return 1'000; }\n"
    wb = [wsrc[o:c + 1] for o, c in po.function_bodies(wsrc)]
    expect(len(wb) == 2 and wb[0].endswith('return u8\'b\';\n}') and wb[1] == '{ return 1\'000; }', 'prefixed char literals: bodies %r' % (wb,))
    # (review) two plants of one name: results and run dirs are keyed by name, so one would be lost silently
    try:
        po.combine({'fx.h': 'inline int F() { return 1; }\n'}, [po.Plant('twin', 'fx.h', r'1', '2'), po.Plant('twin', 'fx.h', r'1', '3')])
        expect(False, 'two plants with one name must be refused')
    except ValueError:
        pass

    tmp = tempfile.mkdtemp(prefix='plant_once_selftest_')
    try:
        orig = open(os.path.join(FX, 'fx.h'), encoding='utf-8', newline='').read()
        recompiled = []
        clang = find_clang()

        def recompile(p):
            recompiled.append(p.name)
            if p.literal:
                new, n = re.subn(p.pat, lambda m: p.rep, orig, count=1)
            else:
                new, n = re.subn(p.pat, p.rep, orig, count=1)
            inc = os.path.join(tmp, 'rc_' + p.name)
            os.makedirs(inc)
            with open(os.path.join(inc, 'fx.h'), 'w', encoding='utf-8', newline='') as f:
                f.write(new)
            exe = os.path.join(inc, 'u.exe')
            r = subprocess.run([clang] + FLAGS + ['-I', inc, os.path.join(FX, 'fx_unit.cpp'), '-o', exe], capture_output=True, text=True)
            if r.returncode != 0:
                return False, 'fx_unit did not compile: ' + (r.stdout + r.stderr)[-200:]
            r = subprocess.run([exe], capture_output=True, text=True)
            if r.returncode == 0:
                return False, 'the unit still PASSES'
            return verdict(r.returncode, r.stdout), (r.returncode, r.stdout)

        logs = []
        spec = po.Spec(unit_cpp=os.path.join(FX, 'fx_unit.cpp'), headers={'fx.h': os.path.join(FX, 'fx.h')}, plants=PLANTS, flags=FLAGS,
                       tmp=os.path.join(tmp, 'once'), log=logs.append, jobs=2)
        results, fallback = po.run_once(spec, verdict, recompile)
        how = {n: r[0] for n, r in results.items()}
        for n in ('clamp_expr', 'clamp_below_deleted', 'sum_init_one', 'sum_init_two', 'add_minus', 'box_ctor', 'literal_rep'):
            expect(how.get(n) == 'run-time' and results[n][1], '%s must be caught at run time, got %r' % (n, results.get(n, (None,))[:2]))
        expect(spec.stats.get('id0_pass') and spec.stats.get('shipped_pass'), 'shipped alone and the combined exe at id 0 must PASS')
        expect(how.get('limit_eleven') == 'recompile' and results['limit_eleven'][1], 'a namespace constexpr plant goes to the fallback and is caught there')
        expect(how.get('twice_off') == 'recompile' and results['twice_off'][1], 'the constexpr function in a static_assert is demoted, then caught by recompile')
        expect('twice_off' in spec.stats.get('demoted', []), 'twice_off must be demoted by the trial compile, not by the generator')
        expect(how.get('clamp_broken') == 'recompile' and not results['clamp_broken'][1] and 'did not compile' in str(results['clamp_broken'][2]),
               'a plant that does not compile is reported "did not compile": %r' % (results.get('clamp_broken'),))
        expect('clamp_broken' in spec.stats.get('demoted', []), 'clamp_broken must be demoted by the trial compile')
        expect(how.get('unchecked_off') == 'recompile' and not results['unchecked_off'][1] and 'still PASSES' in str(results['unchecked_off'][2]),
               'a plant the fixture misses must be re-run by recompile and reported as not caught: %r' % (results.get('unchecked_off'),))
        expect(how.get('gone') == 'no-match' and 'gone' not in recompiled, 'a pattern that matches nothing is reported no-match without a build')
        expect(set(recompiled) == {'limit_eleven', 'twice_off', 'clamp_broken', 'unchecked_off'}, 'recompile ran for %r' % sorted(recompiled))
        expect(set(fallback) == {'limit_eleven', 'twice_off', 'clamp_broken', 'unchecked_off'}, 'fallback list is %r' % fallback)
        expect(spec.stats.get('compiles') == 3, 'compiles: %r (shipped + combined + one trial recompile = 3)' % spec.stats.get('compiles'))
        expect(not spec.stats.get('abandoned'), 'the unit must not be abandoned')

        # (review) the fx2 unit: every plant but the control must be decided by the recompile path
        orig2 = open(os.path.join(FX, 'fx2.h'), encoding='utf-8', newline='').read()
        recompiled2 = []

        def recompile2(p):
            recompiled2.append(p.name)
            new, n = re.subn(p.pat, p.rep, orig2, count=1)
            inc = os.path.join(tmp, 'rc2_' + p.name)
            os.makedirs(inc)
            with open(os.path.join(inc, 'fx2.h'), 'w', encoding='utf-8', newline='') as f:
                f.write(new)
            exe = os.path.join(inc, 'u.exe')
            r = subprocess.run([clang] + FLAGS + ['-I', inc, os.path.join(FX, 'fx2_unit.cpp'), '-o', exe], capture_output=True, text=True)
            if r.returncode != 0:
                return False, 'fx2_unit did not compile: ' + (r.stdout + r.stderr)[-200:]
            r = subprocess.run([exe], capture_output=True, text=True)
            if r.returncode == 0:
                return False, 'the unit still PASSES'
            return True, (r.returncode, r.stdout)

        spec2 = po.Spec(unit_cpp=os.path.join(FX, 'fx2_unit.cpp'), headers={'fx2.h': os.path.join(FX, 'fx2.h')}, plants=PLANTS2, flags=FLAGS,
                        tmp=os.path.join(tmp, 'once2'), log=logs.append, jobs=2)
        res2, _ = po.run_once(spec2, lambda rc, out: rc != 0, recompile2)
        how2 = {n: r[0] for n, r in res2.items()}
        expect(spec2.stats.get('id0_pass') and spec2.stats.get('shipped_pass'), 'fx2: shipped alone and the combined exe at id 0 must PASS')
        expect(how2.get('param_control') == 'run-time' and res2['param_control'][1], 'fx2: the control plant is caught at run time: %r' % (res2.get('param_control'),))
        expect(how2.get('crash_exit') == 'recompile' and res2['crash_exit'][1],
               'a run that ends with a crash code is not a run-time catch; the recompile decides (and catches it): %r' % (res2.get('crash_exit'),))
        for n in ('param_redeclared', 'brace_split', 'consteval_runtime'):
            expect(how2.get(n) == 'recompile' and not res2[n][1] and 'did not compile' in str(res2[n][2]),
                   '%s does not compile alone: it must be reported "did not compile", got %r' % (n, res2.get(n)))
        expect('param_redeclared' in spec2.stats.get('demoted', []), 'param_redeclared must be demoted by its -Wshadow warning in the trial compile')
        expect('uninit_read' in spec2.stats.get('demoted', []) and how2.get('uninit_read') == 'recompile',
               'uninit_read must be demoted by its uninitialised-read warning: %r / %r' % (spec2.stats.get('demoted'), res2.get('uninit_read')))
        expect('brace_split' not in spec2.stats.get('demoted', []), 'brace_split must go to the fallback in the generator, not reach the compile')
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    for f in fails:
        print('FAIL ' + f)
    print('plant_once_selftest: %s' % ('OK' if not fails else '%d failure(s)' % len(fails)))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
