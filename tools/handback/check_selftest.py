#!/usr/bin/env python3
"""Selftest of tools\\handback\\check.py (lane HBK): scratch git repos in a temp folder, stub lints planted in them, no compile, no game.
  py -I tools\\handback\\check_selftest.py            (HBK_DIR=<folder holding check.py> tests another copy, e.g. the first version: its false-PASS rows must fail)
Part 1, the rules (each planted slip MUST give its verdict, each clean twin must not). Part 2, the FALSE-PASS table: for each check one realistic slip
of its class that the first version (commit b9f00b5f0..ce03f8671) passed; each must now FAIL (or BLOCKED / flagged where that is the right answer)."""
# entry: run_checks
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.environ.get('HBK_DIR', HERE))
import check as hb   # noqa: E402
try:
    import cvar_pp as CVAR_PP   # noqa: E402  (LNK round 3: the real-preprocessor cvar judge; absent in a copy older than it)
except ImportError:
    CVAR_PP = None

FAILS = []
ENV = dict(os.environ, GIT_AUTHOR_NAME='t', GIT_AUTHOR_EMAIL='t@t', GIT_COMMITTER_NAME='t', GIT_COMMITTER_EMAIL='t@t')
TMP = None


def ok(cond, text):
    print(('ok    ' if cond else 'FAIL  ') + text, flush=True)
    if not cond:
        FAILS.append(text)


def git(root, *a):
    r = subprocess.run(['git', '-C', root] + list(a), env=ENV, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(r.stderr)
    return r.stdout.strip()


def put(root, rel, text, binary=False):
    p = os.path.join(root, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, 'wb') as f:
        f.write(text if binary else text.encode('utf-8'))


def args(**kw):
    return argparse.Namespace(allow_delete=kw.get('allow_delete', []), skip_compile=kw.get('skip_compile', True))


def verdicts(root, base, names, budget_s=50, **kw):
    try:
        res, summ = hb.run_checks(root, base, args(**kw), names, out=lambda *_a: None, budget_s=budget_s)
    except TypeError:   # the first version has no budget_s
        res, summ = hb.run_checks(root, base, args(**kw), names, out=lambda *_a: None)
    return {r.name: r for r in res}, summ


_N = [0]


def repo(name):
    _N[0] += 1
    r = os.path.join(TMP, '%02d_%s' % (_N[0], name))
    os.makedirs(r)
    git(r, 'init', '-q', '-b', 'main')
    return r


def commit(r, msg):
    git(r, 'add', '-A')
    git(r, 'commit', '-qm', msg)
    return git(r, 'rev-parse', 'HEAD')


PASS_LINT = 'import sys\nprint("stub lint: 0 violation(s)")\n'


def stub_lints(r, overrides=None):
    """Every lint b.lints / a.build_lists may trigger, as a passing stub (the tree's own copy wins over this checkout's)."""
    for rel in ('tools/scene/dev_cvar_lint.py', 'tools/scene/switch_defined_lint.py', 'tools/gate/build_list_lint.py', 'tools/scene/getenv_lint.py',
                'tools/renderer/rule_register_lint.py', 'tools/renderer/census_flush_lint.py', 'tools/scene/census_reach_lint.py', 'tools/scene/off_census_lint.py'):
        put(r, rel, (overrides or {}).get(rel, PASS_LINT))


V_LINT = ('import os, sys\nroot = sys.argv[sys.argv.index("--root") + 1]\nbad = 0\n'
          'for f in sorted(os.listdir(os.path.join(root, "src"))):\n'
          '    t = open(os.path.join(root, "src", f)).read()\n'
          '    if "CRASH" in t: raise RuntimeError("lint bug")\n'
          '    if "BAD" in t: print("VIOLATION src/%s:1: BAD read without IsDevBuild()" % f); bad += 1\n'
          'print("%d violation(s)" % bad)\nsys.exit(1 if bad else 0)\n')


# ------------------------------------------------------------------------------------------------------------------------- part 1: rules

def t_deleted():
    r = repo('del')
    put(r, 'docs/a.md', '# a\n'); put(r, 'docs/b.md', '# b\n')
    base = commit(r, 'c1')
    os.remove(os.path.join(r, 'docs', 'b.md'))
    v, _ = verdicts(r, base, ['a.deleted'])
    ok(v['a.deleted'].verdict == 'FAIL' and 'docs/b.md' in v['a.deleted'].reason, '1 a deleted file FAILS a.deleted')
    v, _ = verdicts(r, base, ['a.deleted'], allow_delete=['docs\\b.md'])
    ok(v['a.deleted'].verdict == 'PASS', '1 --allow-delete naming it PASSES')
    hb.RULES['deleted'] = False
    try:
        v, _ = verdicts(r, base, ['a.deleted'])
    finally:
        hb.RULES['deleted'] = True
    ok(v['a.deleted'].verdict == 'PASS', '1 planted: with the delete rule switched off INSIDE the check the delete is missed (the re-find can fail)')


OLD = '  std::snprintf(e, sizeof e, "[scene][maps][e2] plane made %llu of %llu seen", made, seen);\n'
NEW = '  std::snprintf(e, sizeof e, "[scene][maps][e2] plane made %llu (draws %u; seen %llu)", made, draws, seen);\n'


def t_stale_basic():
    r = repo('stale')
    put(r, 'src/m.cpp', 'void A() {\n' + OLD + '}\n')
    commit(r, 'msf')
    put(r, 'src/m.cpp', 'void A() {\n' + NEW + '}\n')
    fix = commit(r, 'fix: restore the census shape')
    git(r, 'checkout', '-q', '-b', 'builder')
    put(r, 'src/m.cpp', 'void A() {\n' + OLD + '}\nvoid B() {}\n')
    commit(r, 'builder work')
    v, _ = verdicts(r, fix, ['a.stale_base'])
    ok(v['a.stale_base'].verdict == 'FAIL' and fix[:9] in v['a.stale_base'].reason, '2 a silent revert of the base fix FAILS a.stale_base')
    git(r, 'commit', '-q', '--allow-empty', '-m', 'mentions %s but excuses nothing' % fix[:9])
    v, _ = verdicts(r, fix, ['a.stale_base'])
    ok(v['a.stale_base'].verdict == 'FAIL', '2 a message naming the sha WITHOUT "revert" in a commit touching the file excuses nothing')
    put(r, 'src/m.cpp', 'void A() {\n' + OLD + '}\nvoid B() { }\n')
    commit(r, 'revert %s on purpose: the reader moved' % fix[:9])
    v, _ = verdicts(r, fix, ['a.stale_base'])
    ok(v['a.stale_base'].verdict == 'PASS', '2 "revert <sha>" in a commit that touches the file excuses it (%s)' % v['a.stale_base'].verdict)
    r2 = repo('stale_ok')
    put(r2, 'src/m.cpp', 'void A() {\n' + NEW + '}\n')
    b2 = commit(r2, 'c1')
    put(r2, 'src/m.cpp', 'void A() {\n' + NEW.replace('seen %llu', 'seen %llu; held %u') + '}\n')
    v, _ = verdicts(r2, b2, ['a.stale_base'])
    ok(v['a.stale_base'].verdict == 'PASS', '2 an ordinary edit of a base line PASSES (%s)' % v['a.stale_base'].verdict)


def t_control():
    r = repo('ctl')
    put(r, 'src/x.h', b'// old \x08 line\nint a;\n', binary=True)
    base = commit(r, 'c1')
    put(r, 'src/x.h', b'// old \x08 line\nint a;\nint b;\n', binary=True)
    v, _ = verdicts(r, base, ['a.control_chars'])
    ok(v['a.control_chars'].verdict == 'PASS', '3 a control char on an UNTOUCHED line is not yours')
    put(r, 'src/x.h', b'// old \x08 line\nint a;\nint b; // path out\x08uild\n', binary=True)
    v, _ = verdicts(r, base, ['a.control_chars'])
    ok(v['a.control_chars'].verdict == 'FAIL' and 'src/x.h:3' in v['a.control_chars'].reason, '3 a 0x08 on an added line FAILS')


CMAKE = 'set(WWE2K14_SOURCES\n  src/start.cpp\n  src/def.cpp\n)\n'
KS = '#pragma once\n#ifndef WWE2K14_KEEP_STREAM\n#define WWE2K14_KEEP_STREAM 1\n#endif\n#define WWE2K14_STREAM() (WWE2K14_KEEP_STREAM)\n'


def cvar_repo(name):
    r = repo(name)
    put(r, 'CMakeLists.txt', CMAKE)
    put(r, 'src/keep_stream.h', KS)
    put(r, 'tools/scene/keep_stream_off_sources.txt', '# none\n')
    put(r, 'tools/scene/keep_stream_sources.txt', '# none\n')
    put(r, 'src/def.cpp', '#include "keep_stream.h"\nREXCVAR_DEFINE_BOOL(wwe2k14_y, false, "wwe2k14", "y");\n')
    put(r, 'src/decl.h', '#pragma once\nREXCVAR_DECLARE(bool, wwe2k14_y);\n')
    put(r, 'src/start.cpp', '#include "keep_stream.h"\n#include "decl.h"\nvoid Start() {\n  bool y = REXCVAR_GET(wwe2k14_y);\n}\n')
    stub_lints(r)
    return r, commit(r, 'c1')


def t_cvar():
    r, base = cvar_repo('cvar')
    bad = ('#include "keep_stream.h"\n#include "decl.h"\n#if !WWE2K14_STREAM()\nREXCVAR_DEFINE_BOOL(wwe2k14_x, true, "wwe2k14", "x");\n#endif\n'
           'void Start() {\n  bool y = REXCVAR_GET(wwe2k14_y);\n  bool x = REXCVAR_GET(wwe2k14_x);\n}\n')
    put(r, 'src/start.cpp', bad)
    v, _ = verdicts(r, base, ['b.cvar_builds', 'c.defaults'])
    # (LNK round 3 message: "src/start.cpp (ON game build): reads wwe2k14_x, but nothing in that unit declares or defines it"; before: "(ON, ON game build)")
    ok(v['b.cvar_builds'].verdict == 'FAIL' and re.search(r'\(ON(, ON)? game build\)', v['b.cvar_builds'].reason) is not None and 'wwe2k14_x' in v['b.cvar_builds'].reason,
       '4 a cvar defined OFF-only and read unguarded FAILS b.cvar_builds in ON (%s)' % v['b.cvar_builds'].reason[:120])
    ok(any('wwe2k14_x defaults to true' in n for n in v['c.defaults'].notes), '4 c.defaults names the new default-true cvar as a NOTE')
    put(r, 'src/start.cpp', bad.replace('  bool x = REXCVAR_GET(wwe2k14_x);\n', '#if !WWE2K14_STREAM()\n  bool x = REXCVAR_GET(wwe2k14_x);\n#endif\n'))
    v, _ = verdicts(r, base, ['b.cvar_builds'])
    ok(v['b.cvar_builds'].verdict == 'PASS', '4 the same read under #if !WWE2K14_STREAM() PASSES (%s: %s)' % (v['b.cvar_builds'].verdict, v['b.cvar_builds'].reason[:100]))


def t_cvar_at_base():
    """A link error already at base is not the builder's: a NEW reader of another cvar in the same file passes."""
    r = repo('cvar_base')
    put(r, 'CMakeLists.txt', CMAKE)
    put(r, 'src/keep_stream.h', KS)
    put(r, 'tools/scene/keep_stream_off_sources.txt', '# none\n')
    put(r, 'tools/scene/keep_stream_sources.txt', '# none\n')
    put(r, 'src/def.cpp', '#include "keep_stream.h"\n#if !WWE2K14_STREAM()\nREXCVAR_DEFINE_BOOL(wwe2k14_old, true, "w", "o");\n#endif\nREXCVAR_DEFINE_BOOL(wwe2k14_z, true, "w", "z");\n')
    put(r, 'src/start.cpp', '#include "keep_stream.h"\nREXCVAR_DECLARE(bool, wwe2k14_old);\nvoid Start() {\n  bool o = REXCVAR_GET(wwe2k14_old);\n}\n')
    stub_lints(r)
    base = commit(r, 'c1 (already broken in ON: wwe2k14_old)')
    put(r, 'src/start.cpp', '#include "keep_stream.h"\nREXCVAR_DECLARE(bool, wwe2k14_old);\nREXCVAR_DECLARE(bool, wwe2k14_z);\nvoid Start() {\n'
        '  bool o = REXCVAR_GET(wwe2k14_old);\n  bool z = REXCVAR_GET(wwe2k14_z);\n}\n')
    git(r, 'add', '-A')
    # the diff touches wwe2k14_z only; force wwe2k14_old into the judged set by adding an #if line (every cvar of the file is then judged)
    put(r, 'src/start.cpp', '#include "keep_stream.h"\n#if 1\n#endif\nREXCVAR_DECLARE(bool, wwe2k14_old);\nREXCVAR_DECLARE(bool, wwe2k14_z);\nvoid Start() {\n'
        '  bool o = REXCVAR_GET(wwe2k14_old);\n  bool z = REXCVAR_GET(wwe2k14_z);\n}\n')
    v, _ = verdicts(r, base, ['b.cvar_builds'])
    ok(v['b.cvar_builds'].verdict == 'PASS' and any('at base too' in n for n in v['b.cvar_builds'].notes),
       '4 a link error already at base (wwe2k14_old) is a NOTE, not the builder\'s FAIL (%s: %s)' % (v['b.cvar_builds'].verdict, v['b.cvar_builds'].reason[:120]))


HOOKSRC = {'src/assets/build.h': 'namespace ab {\ninline unsigned SlotOwner(unsigned d) {\n  return d + 7;\n}\ninline unsigned DescOfBody(unsigned b) {\n  return b * 2;\n}\n}\n',
           # wrestler_hook.h holds NO hook macro (as in the real tree): OnBody is hook code because its only caller is a REX_HOOK_RAW body
           'src/scene_live/wrestler_hook.h': '#include "../assets/build.h"\ninline void OnBody(unsigned d) {\n  if (d) {\n    unsigned s = ab::SlotOwner(d);\n    (void)s;\n  }\n}\n',
           # a hook body in a file without "hook" in its name (round 3 item 1)
           'src/draw_origin.h': '#include "scene_live/wrestler_hook.h"\nREX_HOOK_RAW(sub_82000000) {\n  const unsigned d = ab::DescOfBody(ctx.r3.u32);\n  OnBody(d);\n}\n'}


def unit_repo(name):
    r = repo(name)
    for k, t in HOOKSRC.items():
        put(r, k, t)
    return r, commit(r, 'c1')


def t_units():
    r, base = unit_repo('units')
    put(r, 'tools/scene/x_unit.cpp', '#include "../../src/assets/build.h"\nint main() { return 0; }\n')
    v, _ = verdicts(r, base, ['e.units'])
    ok(v['e.units'].verdict == 'PASS' and any('no `// entry' in n for n in v['e.units'].notes), '5 a new unit with no entry line is a NOTE (%s)' % v['e.units'].verdict)
    put(r, 'tools/scene/x_unit.cpp', '#include "../../src/scene_live/wrestler_hook.h"\nint main() { OnBody(1); return 0; }\n')
    v, _ = verdicts(r, base, ['e.units'])
    ok(v['e.units'].verdict == 'FAIL' and 'REX_HOOK_RAW' in v['e.units'].reason,
       '5 a unit calling the function a hook body calls, with no entry line naming it, FAILS (it skips the body\'s DescOfBody) (%s)' % v['e.units'].reason[:120])
    put(r, 'tools/scene/x_unit.cpp', '// entry: ab::SlotOwner\n#include "../../src/assets/build.h"\nint main() { return ab::SlotOwner(1) == 8 ? 0 : 1; }\n')
    v, _ = verdicts(r, base, ['e.units'])
    ok(v['e.units'].verdict == 'FAIL', '5 an entry naming a function only a hook calls FAILS (ARM2 class) (%s)' % v['e.units'].reason[:120])
    put(r, 'tools/scene/x_unit.cpp', '// entry: OnBody\n#include "../../src/scene_live/wrestler_hook.h"\nint main() { OnBody(1); return 0; }\n')
    v, _ = verdicts(r, base, ['e.units'])
    ok(v['e.units'].verdict == 'PASS', '5 an entry naming the hook, which the unit calls, PASSES (%s: %s)' % (v['e.units'].verdict, v['e.units'].reason[:100]))


def t_pp():
    # LNK round 3: the Python preprocessor model (PPFile / possible) is gone; the cvar judge reads clang's own text. The scanner of that text:
    if CVAR_PP is not None:
        text = (b'# 1 "C:/t/src/a.cpp"\n#define WRAP(n) inline const bool& FLAGS_##n##_storage_() { static bool v; return v; }\n#define READS() REXCVAR_GET(wwe2k14_r)\n'
                b'WRAP(wwe2k14_w)\nREXCVAR_DECLARE(std::string, wwe2k14_a);\nx = REXCVAR_GET(wwe2k14_b) + READS();\nREXCVAR_DEFINE_BOOL(wwe2k14_c, true, "g", "h");\n'
                b'const char* s = "REXCVAR_GET(wwe2k14_str)";\n# 1 "C:/sdk/include/rex/clock.h" 1\nREXCVAR_DECLARE(bool, clock_x);\n')
        r = CVAR_PP.scan(text, lambda p: '/src/' in p.replace('\\', '/'))
        ok(r['decl'] == ['clock_x', 'wwe2k14_a'] and r['read'] == ['wwe2k14_b', 'wwe2k14_r'] and r['def'] == {'wwe2k14_c': 1} and r['inline'] == ['wwe2k14_w']
           and r['ext'] == ['clock_x'], '6 cvar_pp.scan: DECLARE(std::string, n), GET, a GET inside a macro, DEFINE_*, a FLAGS_##n##_storage_ wrapper, no string literal, the SDK\'s own '
                                       'declaration (%s)' % {k: r[k] for k in ('decl', 'read', 'def', 'inline', 'ext')})
    ok(hb.cvar_names('REXCVAR_DECLARE(std::string, wwe2k14_a); x = REXCVAR_GET(wwe2k14_b); REXCVAR_DEFINE_BOOL(wwe2k14_c, true, "g", "h")')
       == [('DECLARE', 'wwe2k14_a'), ('GET', 'wwe2k14_b'), ('DEFINE_BOOL', 'wwe2k14_c')], '6 cvar_names reads DECLARE(type, name), GET(name), DEFINE_*(name, ...)')


def t_plumbing():
    r = repo('plumb')
    put(r, 'a.txt', 'a\n')
    base = commit(r, 'c1')
    put(r, 'a.txt', 'b\n')
    saved = hb.check_deleted
    hb.check_deleted = lambda ch, a: 1 / 0
    try:
        v, summ = verdicts(r, base, ['a.deleted'])
    finally:
        hb.check_deleted = saved
    ok(v['a.deleted'].verdict == 'BLOCKED' and summ['exit'] == 1, '7 a crashing check is BLOCKED and the exit is 1')
    v, summ = verdicts(r, base, ['a.deleted', 'e.units'])
    ok(summ['exit'] == 0 and v['e.units'].verdict == 'SKIP', '7 PASS + SKIP -> exit 0')


def t_lint_at_base():
    """A violation already at base is not the builder's."""
    r = repo('lint_base')
    put(r, 'src/b.cpp', 'int b = BAD;\n')
    put(r, 'src/a.cpp', 'int a;\n')
    stub_lints(r, {'tools/scene/dev_cvar_lint.py': V_LINT})
    base = commit(r, 'c1')
    put(r, 'src/a.cpp', 'int a = 1;\n')
    v, _ = verdicts(r, base, ['b.lints'])
    ok(v['b.lints'].verdict == 'PASS' and any('at base' in n for n in v['b.lints'].notes), '8 a lint violation already at base is a NOTE, not a FAIL (%s: %s)' % (v['b.lints'].verdict, v['b.lints'].reason[:100]))


P_LINT = ('import os, sys\nroot = sys.argv[sys.argv.index("--root") + 1]\nhits = []\n'
          'for f in sorted(os.listdir(os.path.join(root, "src"))):\n'
          '    for i, ln in enumerate(open(os.path.join(root, "src", f)).read().splitlines(), 1):\n'
          '        if "BAD" in ln: hits.append("src/%s:%d: BAD read without IsDevBuild() (wwe2k14_dev_x)" % (f, i))\n'
          '# a real lint habit: one finding is printed as VIOLATION, several as FAIL: (the prefix changes with the count)\n'
          'for h in hits: print(("VIOLATION " if len(hits) == 1 else "FAIL: ") + h)\n'
          'print("%d violation(s)" % len(hits))\nsys.exit(1 if hits else 0)\n')


def t_lint_prefix_base():
    """Item 15: a base WITH findings. b.cpp's violation moves down a line and its prefix changes (VIOLATION -> FAIL:); only a.cpp's is new."""
    r = repo('lint_prefix')
    put(r, 'src/b.cpp', 'int b = BAD;\n')
    put(r, 'src/a.cpp', 'int a;\n')
    stub_lints(r, {'tools/scene/dev_cvar_lint.py': P_LINT})
    base = commit(r, 'c1')
    put(r, 'src/b.cpp', '// a line above\nint b = BAD;\n')
    put(r, 'src/a.cpp', 'int a = BAD;\n')
    v, _ = verdicts(r, base, ['b.lints'])
    res = v['b.lints']
    ok(res.verdict == 'FAIL' and 'src/a.cpp' in res.reason and not any('src/b.cpp' in d for d in [res.reason] + res.details),
       '15 base with findings: only the new violation (a.cpp) is the builder\'s; b.cpp moved a line and changed prefix (%s: %s)' % (res.verdict, res.reason[:100]))


# ------------------------------------------------------------------------------------------------------------------------- part 2: false-PASS

def fp_lints_prefixed():
    r = repo('fp_lint')
    put(r, 'src/a.cpp', 'int a;\n')
    stub_lints(r, {'tools/scene/dev_cvar_lint.py': V_LINT})
    base = commit(r, 'c1')
    put(r, 'src/a.cpp', 'int a = BAD;\n')
    v, _ = verdicts(r, base, ['b.lints'])
    return v['b.lints'].verdict, 'FAIL', 'a lint printing "VIOLATION src/a.cpp:1 ..." (prefix first) on a new violation'


def fp_lints_crash():
    r = repo('fp_crash')
    put(r, 'src/a.cpp', 'int a;\n')
    stub_lints(r, {'tools/scene/dev_cvar_lint.py': V_LINT})
    base = commit(r, 'c1')
    put(r, 'src/a.cpp', 'int a = CRASH;\n')
    v, _ = verdicts(r, base, ['b.lints'])
    return v['b.lints'].verdict, 'BLOCKED', 'a lint that crashes with a traceback'


def fp_build_lists():
    r = repo('fp_build')
    put(r, 'CMakeLists.txt', 'set(WWE2K14_SOURCES\n  src/x/old.cpp\n)\n')
    put(r, 'src/x/old.cpp', 'int old;\n')
    stub_lints(r)
    base = commit(r, 'c1')
    put(r, 'src/x/new.cpp', 'int fresh;\n')
    put(r, 'tools/gate/lanes/l.json', '{"checks": [{"name": "l.unit", "affects": ["src/x/new.cpp"]}]}\n')
    v, _ = verdicts(r, base, ['a.build_lists'])
    return v['a.build_lists'].verdict, 'FAIL', 'a new src .cpp named only by its lane json (CMake never compiles it)'


def fp_stale_rename():
    r = repo('fp_rename')
    body = ['void A() {', '  int keep_this_line_for_the_test = 1;', OLD.rstrip('\n'), '}']
    put(r, 'old/m.cpp', '\n'.join(body) + '\n')
    commit(r, 'c1')
    os.makedirs(os.path.join(r, 'src'), exist_ok=True)
    git(r, 'mv', 'old/m.cpp', 'src/m.cpp')
    commit(r, 'rename')
    put(r, 'src/m.cpp', '\n'.join(body[:2] + [NEW.rstrip('\n')] + body[3:]) + '\n')
    fix = commit(r, 'fix')
    put(r, 'src/m.cpp', '\n'.join(['void A() {', '  int keep_this_line_for_the_test = 2;', OLD.rstrip('\n'), '}']) + '\n')
    v, _ = verdicts(r, fix, ['a.stale_base'])
    return v['a.stale_base'].verdict, 'FAIL', 'a revert in a file whose history has a rename (blame prints a file-name column)'


def fp_stale_pure_add():
    r = repo('fp_add')
    lines = ['  if (holes) census.holes_counted.fetch_add(1, std::memory_order_relaxed);', '  if (fallback) census.fallback_counted.fetch_add(1, std::memory_order_relaxed);']
    put(r, 'src/m.cpp', 'void A() {\n' + '\n'.join(lines) + '\n}\n')
    commit(r, 'c1')
    put(r, 'src/m.cpp', 'void A() {\n}\n')
    fix = commit(r, 'remove the double count (fix)')
    put(r, 'src/m.cpp', 'void A() {\n' + '\n'.join(lines) + '\n}\n')
    v, _ = verdicts(r, fix, ['a.stale_base'])
    return v['a.stale_base'].verdict, 'FAIL', 'restoring code a base commit deleted (a hunk with no removed line)'


def fp_stale_two_step():
    r = repo('fp_two')
    a_ = '  const char* shape = "plane made %llu of %llu seen";'
    b_ = '  const char* shape = "plane made %llu (draws %u; seen %llu)";'
    b2 = '  const char* shape = "plane made %llu (draws %u; seen %llu; held %u)";'
    put(r, 'src/m.cpp', 'void A() {\n%s\n}\n' % a_); commit(r, 'c1')
    put(r, 'src/m.cpp', 'void A() {\n%s\n}\n' % b_); commit(r, 'C: the fix')
    put(r, 'src/m.cpp', 'void A() {\n%s\n}\n' % b2); base = commit(r, 'D: held')
    put(r, 'src/m.cpp', 'void A() {\n%s\n}\n' % a_)
    v, _ = verdicts(r, base, ['a.stale_base'])
    return v['a.stale_base'].verdict, 'FAIL', 'two-step history: C changed A to B, D changed B to B2, you put A back (blame names D)'


def fp_stale_excuse():
    r = repo('fp_excuse')
    put(r, 'src/m.cpp', 'void A() {\n' + OLD + '}\n'); commit(r, 'c1')
    put(r, 'src/m.cpp', 'void A() {\n' + NEW + '}\n'); fix = commit(r, 'fix')
    git(r, 'checkout', '-q', '-b', 'b')
    put(r, 'docs/n.md', 'see %s\n' % fix[:9]); commit(r, 'notes: %s was reviewed' % fix[:9])
    put(r, 'src/m.cpp', 'void A() {\n' + OLD + '}\n'); commit(r, 'builder work')
    v, _ = verdicts(r, fix, ['a.stale_base'])
    return v['a.stale_base'].verdict, 'FAIL', 'the sha named in an unrelated commit message (no "revert", another file)'


def fp_stale_budget():
    r = repo('fp_budget')
    put(r, 'src/m.cpp', 'void A() {\n' + OLD + '}\n'); base = commit(r, 'c1')
    put(r, 'src/m.cpp', 'void A() {\n' + NEW + '}\n')
    try:
        v, _ = verdicts(r, base, ['a.stale_base'], budget_s=-1)
    except TypeError:
        v, _ = verdicts(r, base, ['a.stale_base'])
    return v['a.stale_base'].verdict, 'BLOCKED', 'an early stop at the time budget'


def fp_cvar_move():
    r, base = cvar_repo('fp_cvar')
    put(r, 'CMakeLists.txt', 'set(WWE2K14_SOURCES\n  src/start.cpp\n)\n')
    put(r, 'tools/scene/keep_stream_off_sources.txt', '# OFF only\nsrc/def.cpp\n')
    v, _ = verdicts(r, base, ['b.cvar_builds'])
    return v['b.cvar_builds'].verdict, 'FAIL', 'a change to the build lists only: the defining unit moves to the OFF build, ON still reads the cvar'


def fp_cvar_unreached_header():
    """LNK review gap 1: a definition in a header counted although no unit of the build includes that header (a link error)."""
    r, base = cvar_repo('fp_cvar_hdr')
    put(r, 'src/defs.h', '#pragma once\nREXCVAR_DEFINE_BOOL(wwe2k14_y, false, "wwe2k14", "y");\n')
    put(r, 'src/def.cpp', '#include "keep_stream.h"\n')
    v, _ = verdicts(r, base, ['b.cvar_builds'])
    return v['b.cvar_builds'].verdict, 'FAIL', 'the definition moves into a header no unit includes (counted before: any header definition passed)'


def fp_cvar_duplicate():
    """LNK review gap 2: two units of one build compile the same definition (a duplicate symbol at link)."""
    r, base = cvar_repo('fp_cvar_dup')
    put(r, 'src/defs.h', '#pragma once\nREXCVAR_DEFINE_BOOL(wwe2k14_y, false, "wwe2k14", "y");\n')
    put(r, 'src/def.cpp', '#include "keep_stream.h"\n#include "defs.h"\n')
    put(r, 'src/start.cpp', '#include "keep_stream.h"\n#include "decl.h"\n#include "defs.h"\nvoid Start() {\n  bool y = REXCVAR_GET(wwe2k14_y);\n}\n')
    v, _ = verdicts(r, base, ['b.cvar_builds'])
    return v['b.cvar_builds'].verdict, 'FAIL', 'a second unit includes the defining header (duplicate definitions were never checked)'


def fp_cmake_comment():
    """LNK review item 3: a .cpp named only in a CMake comment counted as a compiled game unit (a substring test on the CMake text)."""
    r, base = cvar_repo('fp_cmake_comment')
    put(r, 'CMakeLists.txt', CMAKE.replace('  src/def.cpp\n', '  # src/def.cpp retired (its definition went nowhere)\n'))
    v, _ = verdicts(r, base, ['b.cvar_builds'])
    return v['b.cvar_builds'].verdict, 'FAIL', 'the defining unit is left only in a CMake comment: it is not compiled, ON / OFF still read the cvar'


MACRO_DEF = 'REXCVAR_DEFINE_BOOL(wwe2k14_x, false, "wwe2k14", "x");\n'
MACRO_READ = 'void A() {\n  bool x = REXCVAR_GET(wwe2k14_x);\n}\n'


def macro_case(name, files, cmake='set(WWE2K14_SOURCES\n  src/a.cpp\n  src/b.cpp\n)\n'):
    """A tiny tree judged straight through cvar_findings. Since LNK round 3 that is the REAL preprocessor (clang -E -fdirectives-only; the stub flags of cvar_pp.STUB_ENV:
    -I<tree>/src -I<tree>/sdk and the mode define, no SDK needed). -> (findings [(key, message)], notes, blocked)."""
    r = repo(name)
    put(r, 'CMakeLists.txt', cmake)
    put(r, 'src/keep_stream.h', KS)
    put(r, 'tools/scene/keep_stream_off_sources.txt', '# none\n')
    put(r, 'tools/scene/keep_stream_sources.txt', '# none\n')
    for k, t in files.items():
        put(r, k, t)
    notes = []
    try:
        res = hb.cvar_findings(r, {'wwe2k14_x'}, notes)
    except TypeError:   # a version without the notes argument
        res = hb.cvar_findings(r, {'wwe2k14_x'})
    return res[0], notes, (res[2] if len(res) > 2 else [])


def _flagged(found, mode=None):
    return 'FLAGGED' if any(mode is None or k[1] == mode for k, _m in found) else 'clean'


OPTOUT = {'src/cfg.h': '#pragma once\n#ifndef X\n#define X 1\n#endif\n',
          'src/b.cpp': '#define X 0\n#include "cfg.h"\n'}


def fp_macro_optout_false_fail():
    """Reviewer repro 1: a guarded default (cfg.h X=1) and an unrelated unit that opts out (#define X 0) flipped the global macro state each round: a false link FAIL on a.cpp."""
    found, _n, _b = macro_case('fp_mac_ff', dict(OPTOUT, **{'src/defs.h': '#pragma once\n#include "cfg.h"\n#if X\n' + MACRO_DEF + '#endif\n',
                                                         'src/a.cpp': '#include "keep_stream.h"\n#include "defs.h"\n' + MACRO_READ}))
    return _flagged(found), 'clean', 'macro model: a guarded default + an opt-out unit elsewhere (no link error; round 1 gave ON + OFF link findings)'


def fp_macro_optout_false_pass():
    """Reviewer repro 2: the definition under `#if X && WWE2K14_STREAM()`, the read under `#if X`: a real OFF link error that the opt-out unit hid."""
    found, _n, _b = macro_case('fp_mac_fp', dict(OPTOUT, **{'src/defs.h': '#pragma once\n#include "cfg.h"\n#if X && WWE2K14_STREAM()\n' + MACRO_DEF + '#endif\n',
                                                         'src/a.cpp': '#include "keep_stream.h"\n#include "defs.h"\nREXCVAR_DECLARE(bool, wwe2k14_x);\n#if X\n' + MACRO_READ + '#endif\n'}))
    return _flagged(found, 'OFF'), 'FLAGGED', 'macro model: a real OFF link error next to an opt-out unit (round 1: 0 rows, 0 findings)'


def fp_macro_several_values():
    """Item 2: LVL is (1u) when defs.h is read and 2 after it; round 1 pinned the value of whichever define sorted last (and read `(1u) /* c */` as 1 by luck of the fallback)."""
    found, _n, _b = macro_case('fp_mac_val', {'src/lvl1.h': '#pragma once\n#define LVL (1u) /* one */\n',
                                          'src/lvl2.h': '#pragma once\n#undef LVL\n#define LVL 2\n',
                                          'src/defs.h': '#pragma once\n#if LVL == 1\n' + MACRO_DEF + '#endif\n',
                                          'src/a.cpp': '#include "keep_stream.h"\n#include "lvl1.h"\n#include "defs.h"\n#include "lvl2.h"\n' + MACRO_READ,
                                          'src/b.cpp': '#include "keep_stream.h"\n'})
    return _flagged(found), 'clean', 'macro model: a macro with two values in one unit, the definition read while it is 1 (no link error)'


def fp_macro_define_after_include():
    """A unit that #defines the opt-in AFTER including the header: the header's definition is not compiled, a real link error (round 1: "defined somewhere in the unit")."""
    found, _n, _b = macro_case('fp_mac_after', {'src/defs.h': '#pragma once\n#ifdef WANT_X\n' + MACRO_DEF + '#endif\n',
                                            'src/a.cpp': '#include "keep_stream.h"\n#include "defs.h"\n#define WANT_X 1\nREXCVAR_DECLARE(bool, wwe2k14_x);\n' + MACRO_READ,
                                            'src/b.cpp': '#include "keep_stream.h"\n'})
    return _flagged(found), 'FLAGGED', 'macro model: the opt-in #define placed after its include (a real link error)'


def fp_cmake_launcher():
    """Item 3: a header cvar shared by the launcher (another exe) and one game unit is not a duplicate."""
    found, _n, _b = macro_case('fp_launcher', {'src/shared.h': '#pragma once\n' + MACRO_DEF,
                                           'src/a.cpp': '#include "keep_stream.h"\n#include "shared.h"\n' + MACRO_READ,
                                           'src/launcher_main.cpp': '#include "shared.h"\nint main() { return 0; }\n'},
                           cmake='set(WWE2K14_SOURCES\n  src/a.cpp\n)\nadd_executable(wwe2k14_launcher WIN32\n  src/launcher_main.cpp\n)\n')
    return _flagged(found), 'clean', 'a launcher unit (wwe2k14_launcher) and a game unit include one defining header: two exes, not a duplicate'


def fp_pp_blocked():
    """LNK round 3: a unit the preprocessor cannot read (a missing header) and a missing clang are BLOCKED, never PASS (round 2 judged the text it could grep)."""
    r = repo('fp_pp_blocked')
    put(r, 'CMakeLists.txt', 'set(WWE2K14_SOURCES\n  src/a.cpp\n)\n')
    put(r, 'src/keep_stream.h', KS)
    put(r, 'tools/scene/keep_stream_off_sources.txt', '# none\n')
    put(r, 'tools/scene/keep_stream_sources.txt', '# none\n')
    put(r, 'src/a.cpp', '#include "keep_stream.h"\nREXCVAR_DEFINE_BOOL(wwe2k14_x, false, "wwe2k14", "x");\n' + MACRO_READ)
    stub_lints(r)
    base = commit(r, 'c1')
    put(r, 'src/a.cpp', '#include "keep_stream.h"\n#include "gone_missing.h"\nREXCVAR_DEFINE_BOOL(wwe2k14_x, false, "wwe2k14", "x");\n' + MACRO_READ)
    v, _ = verdicts(r, base, ['b.cvar_builds'])
    got = v['b.cvar_builds'].verdict
    if CVAR_PP is not None and got == 'BLOCKED':
        real = CVAR_PP.CLANG
        CVAR_PP.CLANG = None
        try:
            v2, _ = verdicts(r, base, ['b.cvar_builds'])
        finally:
            CVAR_PP.CLANG = real
        got = 'BLOCKED' if v2['b.cvar_builds'].verdict == 'BLOCKED' and 'clang' in v2['b.cvar_builds'].reason else 'no-clang ' + v2['b.cvar_builds'].verdict
    return got, 'BLOCKED', 'a unit that cannot be preprocessed (a missing header), and no clang at all: BLOCKED, never PASS'


# ---- LNK round 3: the reviewer's seven false PASSes of round 2's Python model (each a tiny tree the real preprocessor reads) ----
def fp_pp_values():
    """1. `#if REX_SDK_VERSION >= 2` around the read, the definition under `== 5`, the SDK says 3 (round 2: unknown macros took only 0 / 1, so the read vanished)."""
    found, _n, bl = macro_case('fp_pp_val', {'sdk/rexver.h': '#pragma once\n#define REX_SDK_VERSION 3\n',
                                             'src/defs.h': '#pragma once\n#include <rexver.h>\n#if REX_SDK_VERSION == 5\n' + MACRO_DEF + '#endif\n',
                                             'src/a.cpp': '#include "keep_stream.h"\n#include "defs.h"\nREXCVAR_DECLARE(bool, wwe2k14_x);\n#if REX_SDK_VERSION >= 2\n' + MACRO_READ + '#endif\n',
                                             'src/b.cpp': '#include "keep_stream.h"\n'})
    return _flagged(found), 'FLAGGED', 'value comparisons (>= 2, == 5) on an SDK macro: the read compiles, the definition does not'


def fp_pp_never_defined():
    """2. the definition under `#ifdef NEVER_DEFINED_ANYWHERE`, the read unguarded (round 2: a merely possible definition counted as compiled)."""
    found, _n, bl = macro_case('fp_pp_never', {'src/defs.h': '#pragma once\n#ifdef NEVER_DEFINED_ANYWHERE\n' + MACRO_DEF + '#endif\n',
                                               'src/a.cpp': '#include "keep_stream.h"\n#include "defs.h"\nREXCVAR_DECLARE(bool, wwe2k14_x);\n' + MACRO_READ,
                                               'src/b.cpp': '#include "keep_stream.h"\n'})
    return _flagged(found), 'FLAGGED', 'a definition under a macro nothing defines'


def fp_pp_possible_then_certain():
    """3. cfg.h first reached through `#ifdef SOMETIMES`, then included for certain: WANT is defined, so the `#ifndef WANT` definition is not compiled (round 2 never walked it again)."""
    found, _n, bl = macro_case('fp_pp_maybe', {'src/cfg.h': '#pragma once\n#define WANT 1\n',
                                               'src/defs.h': '#pragma once\n#ifndef WANT\n' + MACRO_DEF + '#endif\n',
                                               'src/a.cpp': '#include "keep_stream.h"\n#ifdef SOMETIMES\n#include "cfg.h"\n#endif\n#include "cfg.h"\n#include "defs.h"\n'
                                                            'REXCVAR_DECLARE(bool, wwe2k14_x);\n' + MACRO_READ,
                                               'src/b.cpp': '#include "keep_stream.h"\n'})
    return _flagged(found), 'FLAGGED', 'a header first reached through a merely possible include, then for certain'


def fp_pp_guardless_twice():
    """4. a header with no guard, included twice with `#define SECOND` between: the second inclusion compiles the definition (round 2 read each file once)."""
    found, _n, bl = macro_case('fp_pp_twice', {'src/inc.h': '#ifdef SECOND\n' + MACRO_DEF + '#endif\n',
                                               'src/a.cpp': '#include "keep_stream.h"\n#include "inc.h"\n#define SECOND 1\n#include "inc.h"\n' + MACRO_READ,
                                               'src/b.cpp': '#include "keep_stream.h"\n'})
    return _flagged(found), 'clean', 'a guard-less header included twice with a define between (no link error)'


def fp_pp_sdk_undef():
    """5. VK_FEATURE comes from an SDK header; our src #undefs it in another unit (round 2 started every name our src #defines / #undefs undefined)."""
    found, _n, bl = macro_case('fp_pp_sdk', {'sdk/vk.h': '#pragma once\n#define VK_FEATURE 1\n',
                                             'src/other.h': '#pragma once\n#undef VK_FEATURE\n',
                                             'src/defs.h': '#pragma once\n#include <vk.h>\n#ifdef VK_FEATURE\n' + MACRO_DEF + '#endif\n',
                                             'src/a.cpp': '#include "keep_stream.h"\n#include "defs.h"\n' + MACRO_READ,
                                             'src/b.cpp': '#include "keep_stream.h"\n#include "other.h"\n'})
    return _flagged(found), 'clean', 'a name an SDK header defines and our src #undefs elsewhere (no link error)'


def fp_pp_include_macro():
    """6. the read sits in a header pulled in by `#include RD_H` (round 2 saw only quoted includes): nothing defines the cvar."""
    found, _n, bl = macro_case('fp_pp_incmac', {'src/rd.h': '#pragma once\nREXCVAR_DECLARE(bool, wwe2k14_x);\n' + MACRO_READ,
                                                'src/a.cpp': '#include "keep_stream.h"\n#define RD_H "rd.h"\n#include RD_H\n',
                                                'src/b.cpp': '#include "keep_stream.h"\n'})
    return _flagged(found), 'FLAGGED', 'a read in a header pulled in by #include MACRO, defined nowhere'


def fp_pp_own_mode_macro():
    """7. a unit that forces `#define WWE2K14_KEEP_STREAM 0` itself: its stream-only definition is not compiled even in the ON build (round 2 took the mode from the build)."""
    found, _n, bl = macro_case('fp_pp_mode', {'src/defs.h': '#pragma once\n#include "keep_stream.h"\n#if WWE2K14_STREAM()\n' + MACRO_DEF + '#endif\n',
                                              'src/a.cpp': '#define WWE2K14_KEEP_STREAM 0\n#include "keep_stream.h"\n#include "defs.h"\nREXCVAR_DECLARE(bool, wwe2k14_x);\n' + MACRO_READ},
                               cmake='set(WWE2K14_SOURCES\n  src/a.cpp\n)\n')
    return _flagged(found, 'ON'), 'FLAGGED', 'a unit forcing its own mode macro to 0: an ON link error'


def fp_dup_key_shrinks():
    """Item 4: a duplicate already at base (3 units) that the diff shrinks to 2 is not the builder's: the key was the unit list, so any change to it looked new."""
    r = repo('fp_dup_shrink')
    put(r, 'CMakeLists.txt', 'set(WWE2K14_SOURCES\n  src/a.cpp\n  src/b.cpp\n  src/c.cpp\n)\n')
    put(r, 'src/keep_stream.h', KS)
    put(r, 'tools/scene/keep_stream_off_sources.txt', '# none\n')
    put(r, 'tools/scene/keep_stream_sources.txt', '# none\n')
    put(r, 'src/defs.h', '#pragma once\n' + MACRO_DEF)
    put(r, 'src/a.cpp', '#include "keep_stream.h"\n#include "defs.h"\n')
    put(r, 'src/b.cpp', '#include "keep_stream.h"\n#include "defs.h"\n')
    put(r, 'src/c.cpp', '#include "keep_stream.h"\n#include "defs.h"\n' + MACRO_READ)
    stub_lints(r)
    base = commit(r, 'c1 (wwe2k14_x already defined in three units)')
    put(r, 'src/c.cpp', '#include "keep_stream.h"\nvoid A() {}\n')
    v, _ = verdicts(r, base, ['b.cvar_builds'])
    return v['b.cvar_builds'].verdict, 'PASS', 'a duplicate at base (3 units) shrinks to 2: still old, not the builder\'s FAIL'


def fp_units_inner():
    r, base = unit_repo('fp_unit')
    put(r, 'tools/scene/y_unit.cpp', '// entry: OnBody\n#include "../../src/scene_live/wrestler_hook.h"\nint main() { return ab::SlotOwner(1) == 8 ? 0 : 1; }\n')
    v, _ = verdicts(r, base, ['e.units'])
    return v['e.units'].verdict, 'FAIL', 'a new unit naming the hook as entry but calling the inner function (ARM2)'


def fp_units_existing():
    r, base0 = unit_repo('fp_unit2')
    put(r, 'tools/scene/z_unit.cpp', '#include "../../src/scene_live/wrestler_hook.h"\nint main() { OnBody(1); return 0; }\n')
    base = commit(r, 'unit')
    put(r, 'tools/scene/z_unit.cpp', '#include "../../src/assets/build.h"\nint main() { return ab::SlotOwner(1) == 8 ? 0 : 1; }\n')
    v, _ = verdicts(r, base, ['e.units'])
    return v['e.units'].verdict, 'FAIL', 'an existing unit (no entry line) changed to call the hook-only function directly'


def fp_lone_cr():
    r = repo('fp_cr')
    put(r, 'tools/harness/keep_list.txt', 'out/a | why\n')
    base = commit(r, 'c1')
    put(r, 'tools/harness/keep_list.txt', b'out/a | why\nout/renderer/re\rlease | the K44 break\n', binary=True)
    v, _ = verdicts(r, base, ['a.control_chars'])
    return v['a.control_chars'].verdict, 'FAIL', 'a lone CR inside a line (keep_list K44)'


def fp_diff_parser():
    r = repo('fp_parse')
    put(r, 'docs/n.md', 'one\n')
    base = commit(r, 'c1')
    put(r, 'docs/n.md', b'one\n++ out\x08uild counter\n', binary=True)
    v, _ = verdicts(r, base, ['a.control_chars'])
    return v['a.control_chars'].verdict, 'FAIL', 'an added line starting "++ " (read as a file header) carrying a 0x08'


def fp_log_reader():
    r = repo('fp_reader')
    put(r, 'src/m.cpp', 'void A() {\n' + NEW + '}\n')
    stub_lints(r)
    for i in range(5):
        name = 'tools/scene/%s_reader.py' % 'abcdz'[i]
        if i < 4:
            put(r, name, 'import sys\n# reads [scene][maps][e2]\nif "--selftest" in sys.argv: print("ok"); sys.exit(0)\n')
        else:
            put(r, name, 'import os, sys\n# reads [scene][maps][e2]\nif "--selftest" in sys.argv:\n    t = open(os.path.join(os.getcwd(), "src", "m.cpp")).read()\n'
                         '    print("selftest PASS" if "(draws %u; seen %llu)" in t else "selftest FAIL: the e2 shape changed"); sys.exit(0 if "(draws %u; seen %llu)" in t else 1)\n')
    base = commit(r, 'c1')
    put(r, 'src/m.cpp', 'void A() {\n' + OLD + '}\n')
    v, _ = verdicts(r, base, ['d.log_readers'])
    return v['d.log_readers'].verdict, 'FAIL', 'a census format change whose 5th reader (past the first version\'s cap of 4) breaks'


def fp_stale_one_line():
    r = repo('fp_one')
    line = '  if (skipped) ++rep.holes;   // NRBF: a skipped visible wrestler counts a hole'
    put(r, 'src/m.cpp', 'void A() {\n  Draw();\n}\n'); commit(r, 'c1')
    put(r, 'src/m.cpp', 'void A() {\n  Draw();\n%s\n}\n' % line); commit(r, 'c2: count holes')
    put(r, 'src/m.cpp', 'void A() {\n  Draw();\n}\n'); fix = commit(r, 'c3: the double count removed (the fix)')
    put(r, 'src/m.cpp', 'void A() {\n  Draw();\n%s\n}\n' % line)
    v, _ = verdicts(r, fix, ['a.stale_base'])
    return v['a.stale_base'].verdict, 'FAIL', 'one line put back that a base commit removed, and nothing else (pure addition)'


def fp_stale_short():
    r = repo('fp_short')
    put(r, 'src/m.cpp', 'bool Ready() {\n  return false;\n}\n'); commit(r, 'c1')
    put(r, 'src/m.cpp', 'bool Ready() {\n  return true;\n}\n'); fix = commit(r, 'fix: ready')
    put(r, 'src/m.cpp', 'bool Ready() {\n    return  false;\n}\n')   # re-indented too
    v, _ = verdicts(r, fix, ['a.stale_base'])
    return v['a.stale_base'].verdict, 'FAIL', 'a short-line fix put back (`return true;` -> `return false;`, re-indented)'


def fp_units_hook_body():
    r, base = unit_repo('fp_hookbody')
    put(r, 'tools/scene/w_unit.cpp', '#include "../../src/assets/build.h"\nint main() { return ab::DescOfBody(2) == 4 ? 0 : 1; }\n')
    v, _ = verdicts(r, base, ['e.units'])
    return v['e.units'].verdict, 'FAIL', 'a unit calling a function whose only caller is a REX_HOOK_RAW body in src/draw_origin.h (no "hook" in the name)'


def fp_base_tree_fails():
    r = repo('fp_nobase')
    put(r, 'src/b.cpp', 'int b = BAD;\n'); put(r, 'src/a.cpp', 'int a;\n')
    stub_lints(r, {'tools/scene/dev_cvar_lint.py': V_LINT})
    base = commit(r, 'c1')
    put(r, 'src/a.cpp', 'int a = 1;\n')
    blocker = os.path.join(TMP, 'a_file_not_a_folder')
    put(TMP, 'a_file_not_a_folder', 'x')
    saved = getattr(hb, 'BASE_DIR', None)
    hb.BASE_DIR = os.path.join(blocker, 'base')   # makedirs under a file fails: the base tree cannot be made
    try:
        v, _ = verdicts(r, base, ['b.lints'])
    finally:
        if saved is not None:
            hb.BASE_DIR = saved
    return v['b.lints'].verdict, 'BLOCKED', 'a lint red at head while the base tree cannot be made'


def fp_cvar_hash():
    r, base = cvar_repo('fp_hash')
    put(r, 'src/r1.h', '#pragma once\ninline bool R1() { return REXCVAR_GET(wwe2k14_q); }\n')
    put(r, 'src/r2.h', '#pragma once\ninline bool R2() { return REXCVAR_GET(wwe2k14_q); }\n')
    put(r, 'src/r3.h', '#pragma once\ninline bool R3() { return REXCVAR_GET(wwe2k14_q); }\n')
    put(r, 'src/start.cpp', '#include "keep_stream.h"\n#include "r1.h"\n#include "r2.h"\n#include "r3.h"\nvoid Start() {}\n')
    prog = ('import sys; sys.path.insert(0, %r); import check as c\n'
            'f = c.cvar_findings(%r, {"wwe2k14_q"})[0]\nprint([x if isinstance(x, str) else x[1] for x in f])\n') % (os.path.dirname(os.path.abspath(hb.__file__)), r)
    outs = set()
    for seed in range(8):
        p = subprocess.run([sys.executable, '-c', prog], capture_output=True, text=True, env=dict(os.environ, PYTHONHASHSEED=str(seed)), timeout=120)
        outs.add(p.stdout.strip() if p.returncode == 0 else 'ERR ' + p.stderr[-200:])
    return ('SAME' if len(outs) == 1 and not next(iter(outs)).startswith('ERR') else 'DIFFERS(%d)' % len(outs)), 'SAME', 'the cvar finding text across 8 processes (hash seeds)'


def fp_reader_base_crash():
    r = repo('fp_rcrash')
    put(r, 'src/m.cpp', 'void A() {\n' + NEW + '}\n')
    stub_lints(r)
    put(r, 'tools/scene/e2_reader.py', 'import os, sys\n# reads [scene][maps][e2]\nif "--selftest" in sys.argv:\n    t = open(os.path.join(os.getcwd(), "src", "m.cpp")).read()\n'
                                       '    if "(draws %u; seen %llu)" in t: raise RuntimeError("reader bug at base")\n    print("selftest FAIL: the e2 shape"); sys.exit(1)\n')
    base = commit(r, 'c1')
    put(r, 'src/m.cpp', 'void A() {\n' + OLD + '}\n')
    v, _ = verdicts(r, base, ['d.log_readers'])
    return v['d.log_readers'].verdict, 'BLOCKED', 'a reader that fails at head and CRASHES at base (cannot tell it is not yours)'


def fp_dirty_tree():
    r = repo('fp_dirty')
    put(r, 'a.txt', 'a\n')
    sha = commit(r, 'c1')
    wt = os.path.join(TMP, 'dirty_wt')
    hb._tree.checkout(r, wt, sha)
    put(wt, 'left_over_untracked.txt', 'from a dead run\n')
    try:
        hb._tree.checkout(r, wt, sha)
        got = 'CHECKED-OUT'
    except Exception as e:
        got = 'REFUSED' if 'changed / untracked' in str(e) else 'ERR %s' % str(e)[:60]
    os.remove(os.path.join(wt, 'left_over_untracked.txt'))
    hb._tree.remove(r, wt)
    return got, 'REFUSED', 'a reused tree with a leftover untracked file (its HEAD matches, its files are not the commit)'


def fp_partial():
    r = repo('fp_partial')
    put(r, 'a.txt', 'a\n')
    base = commit(r, 'c1')
    put(r, 'a.txt', 'b\n')
    v, summ = verdicts(r, base, ['a.deleted'], skip_compile=True)
    return ('FLAGGED' if summ.get('partial') and any(getattr(x, 'by_flag', False) for x in v.values()) else 'silent'), 'FLAGGED', '--only / --skip-compile leave checks out (exit 0 must say partial)'


FALSE_PASS = [('b.lints', fp_lints_prefixed), ('b.lints', fp_lints_crash), ('a.build_lists', fp_build_lists), ('a.stale_base', fp_stale_rename),
              ('a.stale_base', fp_stale_pure_add), ('a.stale_base', fp_stale_two_step), ('a.stale_base', fp_stale_excuse), ('a.stale_base', fp_stale_budget),
              ('b.cvar_builds', fp_cvar_move), ('e.units', fp_units_inner), ('e.units', fp_units_existing), ('a.control_chars', fp_lone_cr),
              ('a.control_chars', fp_diff_parser), ('d.log_readers', fp_log_reader), ('(driver)', fp_partial),
              # round 3 (each seen failing on the round-2 version 7e5af6201)
              ('a.stale_base', fp_stale_one_line), ('a.stale_base', fp_stale_short), ('e.units', fp_units_hook_body), ('b.lints', fp_base_tree_fails),
              ('b.cvar_builds', fp_cvar_hash), ('d.log_readers', fp_reader_base_crash), ('(tree.py)', fp_dirty_tree),
              # lane LNK review (2026-11-20; each seen failing on df9fb9611's check.py first: got PASS)
              ('b.cvar_builds', fp_cvar_unreached_header), ('b.cvar_builds', fp_cvar_duplicate), ('b.cvar_builds', fp_cmake_comment),
              # lane LNK review round 2 (2026-11-20; each seen giving the wrong answer on e5771643a's check.py first)
              ('b.cvar_builds', fp_macro_optout_false_fail), ('b.cvar_builds', fp_macro_optout_false_pass), ('b.cvar_builds', fp_macro_several_values),
              ('b.cvar_builds', fp_macro_define_after_include), ('b.cvar_builds', fp_cmake_launcher),
              ('b.cvar_builds', fp_dup_key_shrinks),
              # lane LNK round 3 (2026-11-20): the real preprocessor; each seen giving the wrong answer on c58e67959's check.py (round 2's Python model) first
              ('b.cvar_builds', fp_pp_blocked), ('b.cvar_builds', fp_pp_values), ('b.cvar_builds', fp_pp_never_defined), ('b.cvar_builds', fp_pp_possible_then_certain),
              ('b.cvar_builds', fp_pp_guardless_twice), ('b.cvar_builds', fp_pp_sdk_undef), ('b.cvar_builds', fp_pp_include_macro), ('b.cvar_builds', fp_pp_own_mode_macro)]


def main():
    global TMP
    # LNK round 3: every scratch tree is preprocessed by the real clang with stub flags (-I<tree>/src -I<tree>/sdk, the mode define): no configured build, no SDK. The child
    # processes (the hash-seed row, the cvar_pp workers) inherit it.
    os.environ['HBK_CVAR_STUB_FLAGS'] = '1'
    TMP = tempfile.mkdtemp(prefix='hbk_selftest_')
    # round 3 item 13: base trees and the cache live in the temp folder: the selftest touches nothing in this repository
    hb.BASE_DIR = os.path.join(TMP, 'hbk_base')
    hb.CACHE_DIR = os.path.join(TMP, 'hbk_base', 'cache')
    print('testing %s' % os.path.abspath(hb.__file__))
    try:
        for t in (t_deleted, t_stale_basic, t_control, t_cvar, t_cvar_at_base, t_units, t_pp, t_plumbing, t_lint_at_base, t_lint_prefix_base):
            try:
                t()
            except Exception as e:
                ok(False, '%s crashed: %s: %s' % (t.__name__, type(e).__name__, str(e)[:200]))
        print('\nFALSE-PASS table (slips the first version passed):')
        print('%-16s %-9s %-9s %s' % ('check', 'want', 'got', 'slip'))
        for chk, fn in FALSE_PASS:
            try:
                got, want, what = fn()
            except Exception as e:
                got, want, what = 'CRASH', '?', '%s: %s: %s' % (fn.__name__, type(e).__name__, str(e)[:120])
            print('%-16s %-9s %-9s %s' % (chk, want, got, what))
            if got != want:
                FAILS.append('false-PASS %s: %s (got %s)' % (chk, what, got))
    finally:
        shutil.rmtree(TMP, ignore_errors=True)
    print('check_selftest: %s (%d failure(s))' % ('PASS' if not FAILS else 'FAIL', len(FAILS)))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
