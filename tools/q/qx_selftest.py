"""qx_selftest: proves the QX1-QX5 queries of `q ours` on a tiny fixture in a TEMP folder (no clang, no game, 0 runs):
  QX1 (worktree freshness) is gone with the index (S110 QTEXT: every answer is the text of the checkout; proven in qsym_selftest.py);
      the two QX1 checks kept here (a name with no C++ hit answers from the tools index, a $variable) do not depend on it;
  QX2 tools index: covers on a python file lists who invokes it and the files that name it; def / refs on a python class attribute, a function, a ps1 function;
  QX3 line: a C++ printf line -> its site + the python regex that parses it (and a regex that does NOT cover it is not listed as matching);
  QX4 lit / includes / includers: a hex literal found word-bounded (0x1234ABCD2 and 980.5 are not hits), includes resolved, includers listed.
  QX5 cvar (define / GET / string / --flag, #if tags, SDK definitions), lit --all / --tools, q sdk def / refs / callers, ps1 function callers, writes inside #if __ANDROID__.
Each check fails loudly with the command and output. Run:  py -I tools/q/qx_selftest.py
"""
import hashlib
import json
import os
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
Q = os.path.join(HERE, 'q.py')
ENV = {k: v for k, v in os.environ.items() if k not in ('WWE_Q_FRESH', 'WWE_Q_FRESH_BUDGET', 'WWE_Q_FRESH_PLANT_MS')}
ENV.update(WWE_Q_OURS_NO_AUTOUPDATE='1', PYTHONIOENCODING='utf-8')


class Fail(Exception):
    pass


def check(cond, what, out=''):
    if not cond:
        raise Fail(f'{what}\n--- output ---\n{out[-1200:]}')
    print(f'PASS  {what}')


def write(root, rel, text):
    p = os.path.join(root, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)


A_CPP = '''#include "a.h"
#include <cstdio>

namespace fx {
int Census(int n) {
    std::printf("[tst][x] widget made %d (n %u; %s)\\n", n, 1u, "ok");
    const unsigned kPhase = 0x1234ABCD;
    const unsigned kOther = 0x1234ABCD2;
    float f = 980.5f;
    return n + 980 + kPhase + kOther + (int)f;
}
}
'''


def sha_size(root, rel):
    with open(os.path.join(root, rel), 'rb') as f:
        b = f.read()
    return hashlib.sha1(b).hexdigest(), len(b)


def build_fixture(root):
    write(root, 'src/a.h', '#pragma once\nint Census(int n);\n')
    write(root, 'src/a.cpp', A_CPP)
    write(root, 'src/b.h', '#pragma once\n#include "a.h"\nint Marker();\n')
    write(root, 'src/e.h', '#pragma once\nint Moved();\n')
    write(root, 'src/d.cpp', ''.join(f'// filler {i}\n' for i in range(1, 17)) + 'int Mover() { return 1; }\n// t1\n// t2\n')
    write(root, 'tools/p.py', '''"""parse the census line"""
import re
import sqlite3

LINE_RX = re.compile(r"widget made (\\d+)")
OTHER_RX = re.compile(r"^\\s*(\\d+)\\b(.*)$")


class Gate:
    def __init__(self):
        self.conn = sqlite3.connect(':memory:')


def verbose(x):
    return x


def parse(line):
    return LINE_RX.search(line)
''')
    write(root, 'tools/r.ps1', 'param([string]$Foo = "x", [switch]$Bar)\nfunction Do-It { & .\\tools\\p.py $Foo }\nDo-It\n')
    for a in (['init', '-q'], ['add', '-A'], ['-c', 'user.name=t', '-c', 'user.email=t@t', 'commit', '-q', '-m', 'fx']):
        subprocess.run(['git', '-C', root] + a, check=True, capture_output=True)


def q(root, *a, env=None):
    t = time.time()
    r = subprocess.run([sys.executable, '-I', Q, 'ours'] + list(a) + ['--root', root], capture_output=True, text=True, encoding='utf-8', errors='replace',
                       env=dict(ENV, **(env or {})), timeout=120)
    return r.stdout + r.stderr, r.stdout, time.time() - t


def qraw(root, *a, env=None):
    """a q command that is not under `ours` (show, py); the checkout under test is the --root of show, and the cwd-independent default of the others"""
    t = time.time()
    extra = ['--root', root] if a[0] == 'show' else (['--root', root] if a[0] == 'py' else [])
    args = list(a)
    if a[0] == 'show':
        args = list(a) + extra
    elif a[0] == 'py':
        args = list(a) + extra
    r = subprocess.run([sys.executable, '-I', Q] + args, capture_output=True, text=True, encoding='utf-8', errors='replace', env=dict(ENV, **(env or {})), timeout=120)
    return r.stdout + r.stderr, r.stdout, time.time() - t


def main():
    tmp = tempfile.mkdtemp(prefix='qx_selftest_')
    try:
        main_root, wt = os.path.join(tmp, 'main'), os.path.join(tmp, 'wt')
        os.makedirs(main_root)
        build_fixture(main_root)
        # ---- QX2 tools index
        out, so, _ = q(main_root, 'covers', 'tools/p.py')
        check('invoked / named by scripts' in so and 'tools/r.ps1' in so, 'QX2 covers tools/p.py lists the ps1 that invokes it', out)
        out, so, _ = q(main_root, 'def', 'Gate.conn')
        check('attr' in so and 'sqlite3.connect' in so and 'tools/p.py:' in so, 'QX2 def of a python self-attribute shows where and what it holds', out)
        out, so, _ = q(main_root, 'refs', 'parse')
        check('tools/p.py' in so and 'LINE_RX' not in so, 'QX2 refs of a python function', out)
        out, so, _ = q(main_root, 'def', 'Do-It')
        check('Do-It' in so and 'r.ps1' in so, 'QX2 def of a ps1 function', out)
        out, so, _ = q(main_root, 'refs', 'p.py')
        check('r.ps1' in so and 'invoke' in so, 'QX2 refs of a script lists the ps1 that invokes it', out)
        # ---- QX3 line
        out, so, _ = q(main_root, 'line', '[tst][x] widget made 3 (n 1; ok)')
        check('src/a.cpp:' in so and 'fx::Census' in so, 'QX3 line finds the printf site and its function', out)
        check('tools/p.py' in so and 'regex matches the line' in so and 'widget made' in so, 'QX3 line finds the python regex that parses it', out)
        body = so.split('parsed / named by tools')[1]
        check('OTHER_RX' not in body and '(\\d+)\\b(.*)$' not in body, 'QX3 a generic regex that matches any digits is NOT listed (planted twin)', out)
        # ---- QX4 lit / includes
        out, so, _ = q(main_root, 'lit', '0x1234ABCD')
        check('src/a.cpp' in so and '(1)' in so and '0x1234ABCD2' not in so.split('\n', 2)[2].replace('const unsigned kPhase = 0x1234ABCD;', ''), 'QX4 lit finds the hex literal word-bounded (0x1234ABCD2 is not a hit)', out)
        out, so, _ = q(main_root, 'lit', '980')
        check('src/a.cpp' in so and '(1)' in so and '980.5' not in so.replace('return n + 980', ''), 'QX4 lit 980 matches 980 but not 980.5f', out)
        out, so, _ = q(main_root, 'includes', 'a.cpp')
        check('#include "a.h"  -> src/a.h' in so and 'cstdio' in so and 'system' in so, 'QX4 includes resolves repo includes and marks system ones', out)
        out, so, _ = q(main_root, 'includers', 'a.h')
        check('src/a.cpp:1' in so and 'src/b.h:2' in so, 'QX4 includers lists every direct includer', out)
        # ---- S105 QB: `line` falls back to text matching for a line built by concatenation; `q show FILE:N-M`; `q py` = ours --tools
        write(main_root, 'src/qb_cat.cpp', '#include <string>\nstatic const char* kQbMsg =\n    "Scheduler is wedged: " "queue " "stalled for";\nvoid QbEmit(int n) { Log(kQbMsg, n); }\n')
        out, so, _ = q(main_root, 'line', 'Scheduler is wedged: queue 7 stalled for 3 frames')
        check('src/qb_cat.cpp:3' in so and 'built by concatenation' in so, 'QB line: a line held in a constant / split literals (no printf call holds it) falls back to a text match of its pieces', out)
        out, so, _ = q(main_root, 'line', '[qb][cat] nothing like this anywhere zzqq')
        check('src/qb_cat.cpp' not in so and 'built by concatenation' not in so, 'QB line: a line nobody prints does not invent a fallback hit (planted twin)', out)
        out, so, _ = qraw(main_root, 'show', 'src/a.cpp:2-3')
        check(so.count('\n') == 4 and '== src/a.cpp:2-3' in so and '-- root:' in so, 'QB show FILE:N-M prints exactly the numbered range, and names the root when it is not the q.py\'s own checkout', out)
        out, so, _ = qraw(main_root, 'show', 'src/a.cpp:0-2')
        check('== src/a.cpp:1-2' in so and '\n0 ' not in so and not any(l.startswith('0 ') for l in so.split('\n')), 'QB show: FILE:0-N starts at line 1, never prints a "line 0"', out)
        out, so, _ = qraw(main_root, 'show', 'a.h:1-2')
        out2, so2, _ = qraw(main_root, 'show', 'xa.h:1-2')
        check('== src/a.h:1-2' in so and 'no such file' in out2, 'QB show: a basename matches whole path components (xa.h is not a.h)', out2)
        write(main_root, 'out/qb_scratch.txt', 'a scratch line\n')
        out, so, _ = qraw(main_root, 'show', 'out/qb_scratch.txt:1')
        check('a scratch line' in so, 'QB show: out\\ text is allowed (policy: logs / scratch reads are 18 % of raw reads)', out)
        write(main_root, 'tools/qb_uni.py', '# café → arrow œ\nx = 1\n')
        r_ = subprocess.run([sys.executable, '-I', Q, 'show', 'tools/qb_uni.py:1-2', '--root', main_root], capture_output=True, env=dict(ENV, PYTHONIOENCODING='ascii'), timeout=120)
        check(r_.returncode == 0 and b'x = 1' in r_.stdout, 'QB show: a legacy console encoding does not crash on non-ASCII text (errors=replace)', (r_.stderr or b'').decode('utf-8', 'replace'))
        for bad in ('generated/x.cpp', './generated/x.cpp', 'src/../generated/x.cpp', 'Generated/x.cpp', 'GAME_DATA/extracted/x.bin', os.path.join(main_root, 'generated', 'x.cpp'), os.path.join(main_root, 'Game_Data', 'x')):
            out, so, _ = qraw(main_root, 'show', bad + ':1-3')
            check('never read through q show' in out and 'q.py decomp' in out and not so.strip().startswith('=='), f'QB show: {bad} is refused (every spelling) and points at q decomp', out)
        out, so, _ = qraw(main_root, 'show', 'a.cpp:2+-1', 'p.py:1')
        check('== src/a.cpp:1-3' in so and '== tools/p.py:1-' in so, 'QB show: a unique basename, FILE:N+-C, FILE:N (context), two files in one call', out)
        out, so, _ = qraw(main_root, 'show', 'src/a.cpp:9999-10001')
        check('beyond the end' in so and 'not clamped' in so, 'QB show: a range past the end of the file is said, not clamped (planted twin)', out)
        # S108 lane D: the first screen is the routing table; the bare verbs of `ours` work
        r_ = subprocess.run([sys.executable, '-I', Q], capture_output=True, text=True, encoding='utf-8', errors='replace', env=dict(ENV), timeout=120)
        check(r_.returncode == 0 and 'WHICH q COMMAND ANSWERS YOUR QUESTION' in r_.stdout and 'q missed' in r_.stdout and 'ours text' in r_.stdout,
              'S108 q with no arguments prints the routing table (question -> command) and names q missed', r_.stdout + r_.stderr)
        for verb_args in (['def', 'Gate.conn'], ['refs', 'parse']):
            r_ = subprocess.run([sys.executable, '-I', Q] + verb_args + ['--root', main_root], capture_output=True, text=True, encoding='utf-8', errors='replace', env=dict(ENV), timeout=120)
            check(r_.returncode == 0 and 'tools/p.py' in r_.stdout, f'S108 bare `q {verb_args[0]}` routes to `q ours {verb_args[0]}`', r_.stdout + r_.stderr)
        r_ = subprocess.run([sys.executable, '-I', Q, 'text', 'Param1', 'ambient', '--root', main_root], capture_output=True, text=True, encoding='utf-8', errors='replace', env=dict(ENV), timeout=120)
        check('text id must be a number' not in (r_.stdout + r_.stderr), 'S108 bare `q text WORD` is the code text search, not the string-table lookup', r_.stdout + r_.stderr)
        # review: the game's own `text` forms stay the game's (an option before the id, -h); a word or a phrase goes to `ours text`
        r_ = subprocess.run([sys.executable, '-I', Q, 'text', '-h'], capture_output=True, text=True, encoding='utf-8', errors='replace', env=dict(ENV), timeout=120)
        check(r_.returncode == 0 and 'game string tables' in r_.stdout, 'S108 review: `q text -h` is still the string-table lookup help', r_.stdout + r_.stderr)
        sys.path.insert(0, HERE)
        import q as qmain
        for av, want in ((['text', '12'], False), (['text', '0x1F'], False), (['text', '--lang', '1', '12'], False), (['text', '--all', '--json', '282'], False),
                         (['text', '-h'], False), (['text'], False), (['text', '12 rounds'], True), (['text', 'PartsBlur'], True), (['text', '12', 'rounds'], True),
                         (['text', '--limit', '5', 'PartsBlur'], True), (['outline', 'src/a.cpp'], True), (['fn', 'sub_82000000'], False), (['db', 'X'], False)):
            check(qmain.bare_ours(av) == want, f'S108 review: bare `q {" ".join(av)}` -> {"ours" if want else "unchanged"}', str(av))
        # review 8: fallback candidates only inside a string literal; comments, .txt / .json and fixtures are not printers; a pair is WEAK unless a second needle backs it
        write(main_root, 'src/qb_cmt.cpp', '// Zorkflib quibblex is only mentioned in a comment\nvoid QbCmt() {}\n')
        write(main_root, 'src/qb_notes.txt', 'Zorkflib quibblex in a txt file\n')
        out, so, _ = q(main_root, 'line', 'Zorkflib quibblex 5 nargle')
        check('qb_cmt.cpp' not in out and 'qb_notes.txt' not in out and 'UNCONFIRMED' not in out, 'QB line: a comment / .txt mention is not a candidate (planted twin)', out)
        write(main_root, 'src/qb_str.cpp', 'const char* kQbPair = "Zorkflib quibblex";\n')
        out, so, _ = q(main_root, 'line', 'Zorkflib quibblex 5 nargle')
        check('src/qb_str.cpp:1' in so and 'UNCONFIRMED' in so and 'WEAK' in so, 'QB line: a pair inside a string literal is listed as UNCONFIRMED and WEAK', out)
        out, so, _ = qraw(main_root, 'py', 'def', 'Gate.conn')
        check('attr' in so and 'tools/p.py:' in so, 'QB py def NAME = the tools python index (self-attribute shown)', out)
        # (6) S110: a name with no C++ text hit answers from the tools index (there is no C++ index to say "found only as a field")
        out, so, _ = q(main_root, 'callers', 'verbose')
        check('tools/p.py' in out, 'QX1 callers of a name with no C++ text hit is answered by the tools index', out)
        out, so, _ = q(main_root, 'def', '$Foo')
        check('r.ps1' in so, 'QX1 def of a PowerShell variable written with its $ finds it', out)
        print('qx_selftest: PASS')
        return 0
    except Fail as e:
        print(f'FAIL  {e}')
        return 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


# ---------------------------------------------------------------- S103 fix round: `q ours line` on pasted lines (review finding 7)

CENSUS_CPP = """#include "a.h"  // "[host][cpu] last" is in this comment
#include <cstdio>
REXCVAR_DEFINE_BOOL(census_x, false, "[host][cpu] last 300 presented frames, per presented frame: build 1.20 ms", "cvar text, not a print");
void Census2(unsigned long long n, double b) {
    std::printf("PREV %d\\n", 1);
    std::printf("[host][cpu] last %llu presented frames, per presented frame: build %.2f ms\\n", n, b);
    // std::printf("[host][cpu] last 300 presented frames, per presented frame: build 1.20 ms\\n");
}
"""
PASSES_CPP = """void Passes(const char* cls, const char* kind, unsigned occ) {
    Log("[rdr][gpu][passes] %s %s #%u image %u %ux%u fmt %u: %.0f us mean / %.0f us p90",
        cls, kind, occ, 1u, 2u, 3u, 4u, 1.0, 2.0);
    Log("[rdr][%s] %s", cls, kind);
}
"""
LAST_DECOYS = "".join('void L%d() { std::printf("last resort %%d\\n", %d); }\n' % (i, i) for i in range(30))
PASSES_PY = r"""import re
STEP_RE = re.compile(r'\[rdr\]\[gpu\]\[passes\] (?P<cls>\w+) (?P<kind>\w+) #(?P<occ>\d+) image (?P<img>\d+) \d+x\d+ fmt \d+: (?P<mean>\d+) us mean')
"""


def qfix_main():
    tmp = tempfile.mkdtemp(prefix='qx_selftest_fix_')
    try:
        root = os.path.join(tmp, 'main')
        os.makedirs(root)
        build_fixture(root)
        write(root, 'src/census.cpp', CENSUS_CPP)
        write(root, 'src/passes.cpp', PASSES_CPP)
        write(root, 'src/decoys.cpp', LAST_DECOYS)
        write(root, 'tools/passes_check.py', PASSES_PY)
        out, so, secs = q(root, 'line', '[host][cpu] last 300 presented frames, per presented frame: build 1.20 ms')
        check('src/census.cpp:6' in so and '(1):' in so.split('parsed')[0], 'FIX7a a pasted line with numbers finds its ONE site (not every "last" in the tree)', out)
        check('decoys.cpp' not in so and 'census.cpp:3' not in so and 'census.cpp:1 ' not in so and 'census.cpp:7' not in so,
              'FIX7c cvar description, // comment and #include-with-comment are not print sites', out)
        check('PREV' not in so, 'FIX7d the format does not carry the previous statement\'s literal', out)
        out, so, _ = q(root, 'line', '[rdr][gpu][passes] main pass #3 image 2 1280x720 fmt 44: 120 us mean / 130 us p90')
        check('src/passes.cpp:2' in so and 'whole line matches' in so and '[rdr][%s]' not in so, 'FIX7b a line whose words came from %s: the whole-line regex finds the format (and not the generic one)', out)
        check('tools/passes_check.py:2' in so, 'FIX7b the parser regex is found through the tag', out)
        out, so, _ = q(root, 'line', '[rdr][gpu][passes] main pass #3 image 2 1280x720 fmt 44: ...')
        check('src/passes.cpp:2' in so and 'tools/passes_check.py:2' in so, 'FIX7b a line pasted cut with "..." still finds site and parser', out)
        out, so, _ = q(root, 'line', '[rdr][gpu][passes]')
        check('src/passes.cpp:2' in so and 'decoys' not in so, 'FIX7 the tag alone lists the tagged print sites', out)
        # ---- finding 9: q_usage_audit buckets and the double-logged tool calls
        sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'harness'))
        import q_usage_audit as qa
        for cmd, want in (('git show abc123:src/a.cpp | grep -n foo', 'read'), ('grep -n "a.*b" src/a.cpp', 'read'), ('find src -type f', 'raw'),
                          ('rg "foo\\.bar" src', 'raw'), ('grep -rn foo src', 'raw'), ('grep -n foo src/a.cpp src/b.h', 'read'), ('py tools/q/q.py ours def X', 'q')):
            got = qa.classify('Bash', {'command': cmd})
            check(got == want, f'FIX9 audit bucket of `{cmd}` is {want} (got {got})')
        rec = {'type': 'assistant', 'message': {'content': [{'type': 'tool_use', 'id': 't1', 'name': 'Bash', 'input': {'command': 'py tools/q/q.py ours def X'}}]}}
        blob = dict(rec, type='api-request-blob')
        jp = os.path.join(tmp, 'agent-x.jsonl')
        with open(jp, 'w', encoding='utf-8') as f:
            f.write(json.dumps(rec) + '\n' + json.dumps(blob) + '\n' + json.dumps(rec) + '\n')
        qn, rn, dn, _, _ = qa.audit(jp)
        check((qn, rn, dn) == (1, 0, 0), f'FIX9 a tool call logged twice (assistant + api-request-blob, same id) counts once: q {qn}')
    except Fail as e:
        print(f'FAIL  {e}')
        return 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print('qx_selftest fix round: PASS')
    return 0


# ---------------------------------------------------------------- S103 QX5: cvar, lit --all / --tools, q sdk, ps1 callers, __ANDROID__ branches

CV_H = """#pragma once
#if WWE2K14_STREAM()
REXCVAR_DEFINE_INT32(fx_step_hz, 0, "fx",
                     "game updates per second (help text one) "
                     "second half");
#else
REXCVAR_DEFINE_BOOL(fx_off_only, false, "fx", "only the stream-free build");
#endif
// fx_step_hz in a comment only
inline int Rate() { return REXCVAR_GET(fx_step_hz) + (int)rex::cvar::GetFlagByName("fx_step_hz").size(); }
"""
AN_CPP = """int g_owner = 0;
void SetOwner() {
#if defined(__ANDROID__)
    g_owner = 1;
#else
    g_owner = 2;
#endif
#if !defined(__ANDROID__)
    int use = g_owner;
#endif
}
"""
SDK_H = """#pragma once
namespace rexx {
class ImGuiBox {
 public:
  virtual void OnCreate(int x);
  int count_ = 0;
};
}
"""
SDK_CPP = """#include "s.h"
REXCVAR_DEFINE_BOOL(fx_sdk_flag, true, "GPU", "an sdk flag");
namespace rexx {
void ImGuiBox::OnCreate(int x) {
  count_ = x;
  if (REXCVAR_GET(fx_sdk_flag)) Helper(x);
}
void Setup(ImGuiBox& b) { b.OnCreate(3); }
}
"""
RUN_PS1 = '$exe = "C:\\x\\pid.exe"  # pid.exe note\n& game --fx_step_hz=120 --other=1\n. .\\lib.ps1\nGet-Fx -A 1\n'


def qs(root, *a):
    r = subprocess.run([sys.executable, '-I', Q, 'sdk'] + list(a) + ['--root', root], capture_output=True, text=True, encoding='utf-8', errors='replace', env=ENV, timeout=120)
    return r.stdout + r.stderr, r.stdout


def qx5_main():
    tmp = tempfile.mkdtemp(prefix='qx_selftest_x5_')
    try:
        root = os.path.join(tmp, 'main')
        os.makedirs(root)
        build_fixture(root)
        write(root, 'src/cv.h', CV_H)
        write(root, 'src/an.cpp', AN_CPP)
        write(root, 'tools/run.ps1', RUN_PS1)
        write(root, 'tools/lib.ps1', 'function Get-Fx { param($A) $A }\n')
        write(root, 'tools/use.ps1', '. .\\lib.ps1\n$r = Get-Fx -A 2\n')
        write(root, 'tools/cv.py', 'FLAGS = "--fx_step_hz=60"\n')
        write(root, 'thirdparty/rexglue-sdk-src/include/s.h', SDK_H)
        write(root, 'thirdparty/rexglue-sdk-src/src/s.cpp', SDK_CPP)
        # ---- 1 cvar
        out, so, _ = q(root, 'cvar', 'fx_step_hz')
        check('src/cv.h:3' in so and 'REXCVAR_DEFINE_INT32' in so and 'default 0' in so and 'second half' in so and 'ON build only (stream)' in so,
              'QX5 cvar: define site, type, default, multi-line help and the #if tag (stream = ON only)', out)
        check('REXCVAR_GET: 1' in so and 'src/cv.h:10' in so and 'by name (a string' in so and 'via GetFlagByName' in so, 'QX5 cvar: REXCVAR_GET and the GetFlagByName("name") string use', out)
        check('tools/run.ps1:2' in so and 'values: 120' in so and 'tools/cv.py:1' in so and 'values: 60' in so, 'QX5 cvar: --NAME=value in a ps1 and a py script, with the values', out)
        check('in comments' in so, 'QX5 cvar: a comment-only mention is counted apart', out)
        out, so, _ = q(root, 'refs', 'fx_off_only')
        check('is a cvar' in out and 'OFF build only (stream-free)' in so, 'QX5 refs / def on a cvar name (no C++ symbol) answers the cvar, tagged OFF only', out)
        out, so, _ = q(root, 'cvar', 'fx_sdk_flag')
        check('[SDK] ' not in so.split('defined')[0] and 'src/s.cpp:2' in so and "category 'GPU'" in so and 'REXCVAR_GET' in so, 'QX5 cvar: the SDK definition and use are found (thirdparty\\rexglue-sdk-src)', out)
        out, so, _ = q(root, 'cvar', 'step_h')
        check('fx_step_hz' in so, 'QX5 cvar: a part of a name resolves to the one cvar containing it', out)
        # ---- 2 lit --all / --tools
        write(root, 'src/many.h', ''.join(f'int v{i} = 0x7A7A7A; // {i}\n' for i in range(6)))
        out, so, _ = q(root, 'lit', '0x7A7A7A')
        check('v2 =' in so and 'v4 =' not in so and '--all' in so, 'QX5 lit default: 3 lines per file and a "more (--all)" note', out)
        out, so, _ = q(root, 'lit', '0x7A7A7A', '--all')
        check('v5 = 0x7A7A7A' in so and 'v4 =' in so, 'QX5 lit --all lists every line of the file', out)
        out, so, _ = q(root, 'lit', 'pid.exe', '--tools', '--all')
        check('tools/run.ps1' in so and '<string>' in so and '<comment>' not in so and 'src/' not in so, 'QX5 lit --tools: a ps1 string literal found (the logged "pid.exe" miss), kind string, scripts only', out)
        # ---- 4 ps1 callers
        out, so, _ = q(root, 'callers', 'Get-Fx')
        check('tools/run.ps1' in so and 'tools/use.ps1:2' in so and '[call]' in so, 'QX5 callers of a ps1 function lists the OTHER scripts that call it (dot-sourced)', out)
        # ---- 5 __ANDROID__ branches
        out, so, _ = q(root, 'writes', 'g_owner')
        check('src/an.cpp:4' in so and '#if defined(__ANDROID__)' in so.replace('#if !(defined(__ANDROID__))', '') or 'defined(__ANDROID__)' in so, 'QX5 writes: a write inside #if __ANDROID__ is listed with its #if chain (S110: text lists every branch)', out)
        out, so, _ = q(root, 'reads', 'g_owner')
        check('src/an.cpp:9' in so and '#if !defined(__ANDROID__)' in so, 'QX5 reads: the !__ANDROID__ branch is listed with its negated #if chain', out)
        # ---- 3 q sdk
        out, so = qs(root, 'update')
        check('2 files' in so and 'definitions' in so, 'QX5 q sdk update builds the index', out)
        out, so = qs(root, 'def', 'OnCreate')
        check('include/s.h:5' in so and 'rexx::ImGuiBox::OnCreate' in so and 'src/s.cpp:4' in so, 'QX5 sdk def: the class-body declaration and the out-of-class definition, qualified', out)
        out, so = qs(root, 'callers', 'OnCreate')
        check('src/s.cpp:8' in so and 'rexx::Setup' in so and 'include/s.h:5  in' not in so, 'QX5 sdk callers: the call site with its function (the declaration is not a caller)', out)
        out, so = qs(root, 'refs', 'fx_sdk_flag')
        check('src/s.cpp:2' in so and 'cvar' in so and 'rexx::ImGuiBox::OnCreate' in so, 'QX5 sdk refs of an SDK cvar name: its definition and its REXCVAR_GET site', out)
        out, so = qs(root, 'def', 'count_')
        check('variable' in so and 'rexx::ImGuiBox::count_' in so, 'QX5 sdk def of a member variable', out)
    except Fail as e:
        print(f'FAIL  {e}')
        return 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print('qx_selftest QX5: PASS')
    return 0


# ---------------------------------------------------------------- S103 QX5 fix round (Opus review findings 1-12): each case failed before its fix

FAILS = []


def soft(cond, what, out=''):
    if cond:
        print(f'PASS  {what}')
    else:
        FAILS.append(what)
        print(f'FAIL  {what}\n--- output ---\n{out[-700:]}')


def git(root, *a):
    subprocess.run(['git', '-C', root, '-c', 'user.name=t', '-c', 'user.email=t@t'] + list(a), check=True, capture_output=True)


def qx5unit():
    import re
    sys.path.insert(0, HERE)
    import qx5
    # 12: preprocessor forms
    lines = ['#if 0', 'dead', '#endif', '#if (WWE2K14_STREAM())', 'paren', '#endif', '#if defined __ANDROID__', 'nop', '#endif', '#ifdef X', 'a', '#elifdef __ANDROID__', 'b', '#endif',
             '#ifdef Y', 'c', '#elifndef __ANDROID__', 'd', '#endif']
    cs = qx5.pp_conds(lines)
    soft('dead' in qx5.tags_of(cs[1]) and qx5.builds_of(qx5.tags_of(cs[1])).startswith('never'), 'FIX12 #if 0 is a dead region (builds: never)', repr(cs[1]))
    soft(qx5.tags_of(cs[4]) == {'stream'}, 'FIX12 #if (WWE2K14_STREAM()) in parentheses is the stream tag', repr(qx5.tags_of(cs[4])))
    soft('android' in qx5.tags_of(cs[7]), 'FIX12 #if defined __ANDROID__ (no parentheses) is the android tag', repr(qx5.tags_of(cs[7])))
    soft('android' in qx5.tags_of(cs[12]), 'FIX12 #elifdef __ANDROID__ is read', repr(cs[12]))
    soft('non-android' in qx5.tags_of(cs[17]), 'FIX12 #elifndef __ANDROID__ is read', repr(cs[17]))
    m = qx5.mask_comments('auto s = R"x(a // b)x"; // real\n')
    soft('a // b' in m and 'real' not in m, 'FIX12 a raw string literal is kept whole and the real comment after it is blanked', m)
    k = dict(qx5.classify_use('see --fx_step_hz in the docs for 120 frames', 'fx_step_hz', 'see --fx_step_hz in the docs for 120 frames', 'x.md'))
    soft(k.get('cmdline') == '', 'FIX12 a prose "--name word" gives no value (only =value counts)', repr(k))
    # 2: REXCVAR_SET
    soft(('set', '') in qx5.classify_use('REXCVAR_SET(fx_a, true);', 'fx_a', 'REXCVAR_SET(fx_a, true);', 'a.cpp'), 'FIX2 REXCVAR_SET(name, v) is a set use')
    soft(('set', '') not in qx5.classify_use('REXCVAR_SET(fx_ab, true);', 'fx_a', 'REXCVAR_SET(fx_ab, true);', 'a.cpp'), 'FIX2 REXCVAR_SET of a longer name is not a use of fx_a')


def qx5fix_main():
    import re
    tmp = tempfile.mkdtemp(prefix='qx_selftest_x5fix_')
    try:
        root = os.path.join(tmp, 'main')
        os.makedirs(root)
        build_fixture(root)
        write(root, 'tools/scene/keep_stream_sources.txt', '# c\nsrc/live.cpp\n')
        write(root, 'tools/scene/keep_stream_off_sources.txt', 'src/off1.cpp\n')
        write(root, 'src/live.cpp', 'REXCVAR_DEFINE_BOOL(fx_rdr_scene, false, "fx", "scene");\nvoid LiveF() { REXCVAR_SET(fx_rdr_scene, true); }\n')
        write(root, 'src/rdr.h', '#pragma once\nREXCVAR_DEFINE_BOOL(fx_rdr_bc, false, "fx", "bc");\n')
        write(root, 'src/both.h', '#pragma once\nREXCVAR_DEFINE_BOOL(fx_both, false, "fx", "both");\n')
        write(root, 'src/inc_user.cpp', '#if WWE2K14_STREAM()\n#include "rdr.h"\n#include "both.h"\n#endif\n')
        write(root, 'src/inc_plain.cpp', '#include "both.h"\n')
        write(root, 'src/off1.cpp', 'REXCVAR_DEFINE_BOOL(fx_dual, false, "fx", "off");\n')
        write(root, 'src/plain.cpp', 'REXCVAR_DEFINE_BOOL(fx_dual, false, "fx", "on");\n')
        write(root, 'src/andcv.cpp', '#if defined(__ANDROID__)\nREXCVAR_DEFINE_BOOL(fx_andr, false, "fx", "a");\n#endif\n#ifdef __ANDROID__\nint A() { return REXCVAR_GET(fx_andr); }\n'
                                     '#else\nint B() { return REXCVAR_GET(fx_andr) + 1; }\n#endif\n')
        write(root, 'src/mac.h', '#pragma once\n#define REXCVAR_DEFINE_BOOL(name, def, cat, help) \\\n    REXCVAR_DEFINE_BOOL_IMPL(name, def)\n')
        write(root, 'src/dup.cpp', 'REXCVAR_DEFINE_BOOL(fx_dup_cv, false, "fx", "d");\n')
        write(root, 'tools/dup.py', 'def fx_dup_cv():\n    return 1\n')
        write(root, 'tools/cv2.py', 'X = "zzmarker77"\n')
        write(root, 'tools/lib.ps1', 'function Get-Fx { param($A) $A }\n')
        write(root, 'tools/use.ps1', '. .\\lib.ps1\n$r = Get-Fx -A 2\n# Get-Fx in a comment\nWrite-Host "Get-Fx is great"\n')
        write(root, 'tools/nosrc.ps1', 'Get-Fx -A 9\n')
        write(root, 'src/andq.cpp', '#if defined(__ANDROID__)\nvoid AQ() { Thing::Frob();\n  other.Frob(); }\n#endif\n')
        git(root, 'add', '-A')
        git(root, 'commit', '-q', '-m', 'fx2')
        # 1 build tags from the file's own #if lines + the lists + includers
        out, so, _ = q(root, 'cvar', 'fx_rdr_scene')
        soft('ON build only (stream)' in so and 'REXCVAR_SET: 1' in so, 'FIX1a/2 a cvar in a keep_stream_sources file is ON only; REXCVAR_SET is found', out)
        out, so, _ = q(root, 'cvar', 'fx_rdr_bc')
        soft('builds: ON build only (stream)' in so, 'FIX1b a header included only under #if WWE2K14_STREAM() is ON only', out)
        out, so, _ = q(root, 'cvar', 'fx_both')
        soft('builds: ON and OFF' in so, 'FIX1b a header with one unconditional includer stays "ON and OFF"', out)
        out, so, _ = q(root, 'cvar', 'fx_dual')
        soft('src/off1.cpp:1' in so and 'OFF build only (stream-free)' in so and re.search(r'src/plain\.cpp:1.*builds: ON build only \(stream\) \(inferred\)', so), 'FIX1c the twin of an OFF-only definition is ON only (inferred)', out)
        # 3 android grouping
        out, so, _ = q(root, 'cvar', 'fx_andr')
        soft('Android only' in so and 'not Android' in so, 'FIX3 cvar sites are tagged and grouped by android / not Android', out)
        # 9 a #define body is not a definition
        out, so, _ = q(root, 'cvar', 'name')
        soft('defined [ours] src/mac.h' not in so, 'FIX9 the macro body REXCVAR_DEFINE_BOOL(name, ...) in a #define is not a cvar definition', out)
        # 10 same-named tools symbol
        out, so, _ = q(root, 'def', 'fx_dup_cv')
        soft('also a tools symbol' in out and 'tools/dup.py' in out, 'FIX10 a cvar answer notes the same-named tools symbol (--tools)', out)
        # 5 ps1 function callers
        out, so, _ = q(root, 'callers', 'Get-Fx')
        soft(re.search(r'tools/use\.ps1:2\s', so) is not None and 'nosrc.ps1' not in so, 'FIX5 only scripts that dot-source the defining file, only command-position lines (not comment / quoted text)', out)
        out, so, _ = q(root, 'writes', 'Get-Fx')
        soft('use.ps1' not in so, 'FIX5 writes of a ps1 function honours kinds (a call is not a write)', out)
        # 6 text commands do not touch the index; 7 android section
        sys.path.insert(0, HERE)
        wt = os.path.join(tmp, 'wt')
        git(root, 'worktree', 'add', '-q', '-b', 'wt5', wt)
        out, so, secs = q(wt, 'cvar', 'fx_rdr_scene')
        soft('REXCVAR_SET: 1' in so and 'main checkout' not in out and secs < 15, f'FIX6 q ours cvar in a worktree answers without the index / overlay ({secs:.1f} s)', out)
        out, so, secs = q(wt, 'lit', 'zzmarker77')
        soft('tools/cv2.py' in so and 'main checkout' not in out, 'FIX6 lit with no C++ hit never opens the index (lazy enclosing-function lookup)', out)
        out, so, _ = q(root, 'refs', 'Thing::Frob')
        l2 = re.search(r'andq\.cpp:2\s.*', so)
        l3 = re.search(r'andq\.cpp:3\s.*', so)
        soft(bool(l2) and bool(l3) and 'name only' not in l2.group(0) and 'name only' in l3.group(0),
             'FIX7 a qualified query: the line holding the qualifier is exact, one with only the name says "name only"', out)
        # 4 / 8 SDK
        sdk = os.path.join(root, 'thirdparty', 'rexglue-sdk-src')
        write(root, 'thirdparty/rexglue-sdk-src/include/v.h', 'namespace rexx {\nclass Vk {\n public:\n  bool Initialize(int x);\n};\n}\n')
        write(root, 'thirdparty/rexglue-sdk-src/src/v.cpp', '#include "v.h"\nnamespace rexx {\nbool Vk::Initialize(int x) { return x != 0; }\nvoid Drive(Vk* vk_) { vk_->Initialize(1); }\n}\n')
        write(root, 'thirdparty/rexglue-sdk-src/include/spv.h', '#if 0\nint BoundZz = 0;\nenum { EndZz };\n#else\nint ElseZz = 2;\n#endif\nint LiveZz = 1;\n')
        write(root, 'thirdparty/rexglue-sdk-src/src/m.cpp', 'namespace rexx {\nstatic_assert_size(Vk, 16);\n}\n')
        write(root, 'thirdparty/rexglue-sdk-src/src/t.cpp', 'namespace rexx {\ntemplate <typename T> struct Wrap { void Run(int x); };\ntemplate <> void Wrap<int>::Run(int x) { (void)x; }\n}\n')
        subprocess.run(['git', '-C', sdk, 'init', '-q'], check=True, capture_output=True)
        git(sdk, 'add', '-A')
        git(sdk, 'commit', '-q', '-m', 'one')
        out, so = qs(root, 'update')
        out, so = qs(root, 'callers', 'Vk::Initialize')
        soft('src/v.cpp:4' in so and 'receiver type unknown' in so, 'FIX4 a qualified callers query keeps member calls through -> / . labelled "receiver type unknown"', out)
        out, so = qs(root, 'def', 'BoundZz')
        soft('0 match' in so, 'FIX8 an #if 0 region defines nothing', out)
        out, so = qs(root, 'def', 'ElseZz')
        soft('1 match' in so, 'FIX8 the #else of an #if 0 is live', out)
        out, so = qs(root, 'def', 'static_assert_size')
        soft('0 match' in so, 'FIX8 a lowercase function-like macro invocation is not a declaration', out)
        out, so = qs(root, 'def', 'Run')
        soft(re.search(r'src/t\.cpp:3\s+\[sdk\]\s+function\s+rexx::Wrap::Run', so) is not None, 'FIX8 a template specialisation definition Wrap<int>::Run is qualified rexx::Wrap::Run', out)
        out, so = qs(root, 'info')
        soft('sdk_head' in so, 'FIX8 sdk info records the SDK HEAD', out)
        write(root, 'thirdparty/rexglue-sdk-src/include/w.h', 'int WNew = 1;\n')
        git(sdk, 'add', '-A')
        git(sdk, 'commit', '-q', '-m', 'two')
        out, so = qs(root, 'def', 'WNew')
        soft('1 match' in so and 'include/w.h' in so and 'refreshed' in so, 'FIX8 / QSD a query refreshes by itself when the SDK moved (a new file is found, no `sdk update` call)', out)
        alt = os.path.join(root, 'thirdparty', 'sdk-wt', 'alpha')
        write(root, 'thirdparty/sdk-wt/alpha/src/z.cpp', 'namespace q1 { void ZzOnly() {} }\n')
        out, so = qs(root, 'update', '--sdk', alt)
        out, so = qs(root, 'def', 'ZzOnly', '--sdk', alt)
        soft('z.cpp' in so, 'FIX8 --sdk PATH indexes and answers for a thirdparty\\sdk-wt tree', out)
        out, so = qs(root, 'def', 'ZzOnly')
        soft('0 match' in so, 'FIX8 the default index does not hold the --sdk tree', out)
        qx5unit()
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if FAILS:
        print(f'qx_selftest QX5 fix round: {len(FAILS)} FAIL')
        for w in FAILS:
            print(f'  FAIL {w}')
        return 1
    print('qx_selftest QX5 fix round: PASS')
    return 0


# ---------------------------------------------------------------- QX5 fix round part 2: lit --tools kinds (finding 11)

def qx5kinds_main():
    import re
    tmp = tempfile.mkdtemp(prefix='qx_selftest_kinds_')
    try:
        root = os.path.join(tmp, 'main')
        os.makedirs(root)
        build_fixture(root)
        write(root, 'tools/k.py', '"""doc with kmark99\nspanning lines kmark99 here"""\nx = 1  # kmark99 in a comment\ny = f"kmark99 {x}"\nkmark99 = 3\n')
        write(root, 'tools/k.ps1', '$a = @"\nkmark99 in a herestring\n"@\n<# block kmark99 #>\n$b = "tick `" kmark99"\nWrite-Host foo#kmark99\n# kmark99 line comment\n$c = 1 # tail kmark99\nkmark99\n')
        out, so, _ = q(root, 'lit', 'kmark99', '--tools', '--all')
        def sect(name):
            res = {}
            blocks = re.split(r'(?m)^  (?=tools/)', so)
            for blk in blocks:
                if blk.startswith(name + ' '):
                    for m in re.finditer(r'(?m)^\s+(\d+): .*?<(\w+)>\s*$', blk):
                        res[int(m.group(1))] = m.group(2)
            return res
        kinds, pk = sect('tools/k.py'), sect('tools/k.ps1')
        soft(kinds.get(1) == 'string' and kinds.get(2) == 'string' and kinds.get(3) == 'comment' and kinds.get(4) == 'string' and kinds.get(5) == 'code',
             'FIX11 python via tokenize: docstring lines 1-2 string, # comment, f-string, bare name code', out + repr(kinds))
        soft(pk.get(2) == 'string' and pk.get(4) == 'comment' and pk.get(5) == 'string' and pk.get(6) == 'code' and pk.get(7) == 'comment' and pk.get(8) == 'comment' and pk.get(9) == 'code',
             'FIX11 ps1 whole-file state: here-string, <# #>, backtick escape inside "", mid-word # is not a comment', out + repr(pk))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if FAILS:
        return 1
    return 0


# ---------------------------------------------------------------- QSD: q sdk covers SDL3 / imgui, builds + refreshes itself, says how old the index is

SDL_FN = (
    'static bool SkipAltGrLeftControl(WPARAM wParam, LPARAM lParam)\n{\n    return wParam == VK_CONTROL;\n}\n'
    'LRESULT WIN_WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)\n{\n    if (SkipAltGrLeftControl(wParam, lParam)) { return 0; }\n    return 1;\n}\n')


def qsd_main():
    tmp = tempfile.mkdtemp(prefix='qx_selftest_qsd_')
    try:
        root = os.path.join(tmp, 'main')
        os.makedirs(root)
        build_fixture(root)
        tp = 'thirdparty/rexglue-sdk-src/'
        sdlc = tp + 'thirdparty/sdl3/src/video/windows/SDL_windowsevents.c'
        write(root, tp + 'src/core.cpp', 'namespace rexx { void CoreOnly(int x) { (void)x; } }\n')
        write(root, sdlc, SDL_FN)
        write(root, tp + 'thirdparty/imgui/backends/imgui_impl_sdl3.cpp', 'void ImGui_ImplSDL3_NewFrame() { int a = 1; (void)a; }\n')
        write(root, tp + 'thirdparty/sdl3/test/skipme.c', 'void SkippedTestOnly(void) {}\n')
        # 1 the index does not exist: the first query builds it (no `sdk update`), finds the SDL symbol, tags the part, prints the index age
        out, so = qs(root, 'def', 'SkipAltGrLeftControl')
        check('1 match' in so and '[sdl3]' in so and 'SDL_windowsevents.c:1' in so, 'QSD q sdk def finds an SDL3 symbol on the first call (index built by itself)', out)
        check('index built' in so and 'ago' in so and 'built just now' in so, 'QSD every answer header says how old the index is (and that it was built just now)', out)
        out, so = qs(root, 'def', 'ImGui_ImplSDL3_NewFrame')
        check('[imgui]' in so and '0 files changed since' in so, 'QSD an imgui symbol is found, tagged [imgui]; the second call re-reads nothing', out)
        out, so = qs(root, 'def', 'SkippedTestOnly')
        check('0 match' in so, 'QSD a third-party test folder is not indexed', out)
        out, so = qs(root, 'def', 'SkipAltGrLeftControl', '--part', 'sdk')
        check('0 match' in so, 'QSD --part sdk drops the SDL3 hit', out)
        out, so = qs(root, 'def', 'SkipAltGrLeftControl', '--part', 'sdl3')
        check('1 match' in so, 'QSD --part sdl3 keeps it', out)
        out, so = qs(root, 'callers', 'SkipAltGrLeftControl')
        check('WIN_WindowProc' in so and '[sdl3]' in so, 'QSD callers: the SDL call site and its function', out)
        # 2 PLANT: a stale file (the SDL source gains a function; a different size) must be re-read on the next call, with no update call
        write(root, sdlc, SDL_FN + 'static void PlantedLaterFn(void) {}\n')
        out, so = qs(root, 'def', 'PlantedLaterFn')
        check('1 match' in so and 'refreshed just now: 1 files re-read' in so, 'QSD PLANT: a deliberately stale file is re-read by the next query (1 file re-read, the new function found)', out)
        out, so = qs(root, 'def', 'PlantedLaterFn')
        check('0 files changed since' in so, 'QSD ... and the call after that re-reads nothing', out)
        # 3 PLANT: a stale file while another writer holds the lock: the answer must say STALE, loudly
        write(root, sdlc, SDL_FN + 'static void PlantedLaterFn(void) {}\nstatic void PlantedLockedFn(void) {}\n')
        sys.path.insert(0, HERE)
        import qsdk
        lk = qsdk.db_path(root) + '.lock'      # S105 D: the schema is in the file name
        with open(lk, 'w') as f:
            f.write('999999')
        env = dict(ENV, WWE_QSDK_LOCK_WAIT='1')
        r = subprocess.run([sys.executable, '-I', Q, 'sdk', 'def', 'PlantedLockedFn', '--root', root], capture_output=True, text=True, encoding='utf-8', errors='replace', env=env, timeout=120)
        check('WARNING STALE' in r.stdout and '0 match' in r.stdout, 'QSD PLANT: a stale file with the writer lock held is reported as WARNING STALE in the header, not answered silently', r.stdout + r.stderr)
        os.remove(lk)
        out, so = qs(root, 'def', 'PlantedLockedFn')
        check('1 match' in so, 'QSD once the lock is gone the next call refreshes', out)
        # 4 info lists the parts
        out, so = qs(root, 'info')
        check('sdl3 1' in so and 'imgui 1' in so and 'sdk 1' in so, 'QSD info lists the parts', out)
    except Fail as e:
        print(f'FAIL  {e}')
        return 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    # known question (tonight's keyboard review): where does SDL drop the fake left Ctrl on AltGr? One q sdk call on the REAL SDK tree (skipped when absent)
    here = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    real = os.path.join(here, 'thirdparty', 'rexglue-sdk-src', 'thirdparty', 'sdl3')
    if not os.path.isdir(real):
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import qx5
        sdk = qx5.sdk_root(here)
        real = os.path.join(sdk, 'thirdparty', 'sdl3') if sdk else ''
    if os.path.isdir(real):
        real_tmp = tempfile.mkdtemp(prefix='qx_real_sdk_')      # S105 D (F-3): a TEMP index for the real tree, never the shared one
        r = subprocess.run([sys.executable, '-I', Q, 'sdk', 'def', 'SkipAltGrLeftControl'], capture_output=True, text=True, encoding='utf-8', errors='replace',
                           env=dict(ENV, WWE_QSDK_DB=os.path.join(real_tmp, 'sdk.real.db')), timeout=300)
        shutil.rmtree(real_tmp, ignore_errors=True)
        if 'SDL_windowsevents.c' not in r.stdout:
            print('FAIL  QSD the real-tree AltGr question: q sdk def SkipAltGrLeftControl did not hit SDL_windowsevents.c\n' + r.stdout + r.stderr)
            return 1
        print('PASS  QSD the real-tree AltGr question: one q sdk def call -> ' + r.stdout.strip().splitlines()[-1].strip())
    else:
        print('SKIP  QSD the real-tree AltGr question (no SDL3 under thirdparty/rexglue-sdk-src in this checkout or the main one)')
    print('qx_selftest QSD: PASS')
    return 0


if __name__ == '__main__':
    rc = main() or qfix_main() or qx5_main()
    rc = rc or qx5fix_main() or qx5kinds_main() or qsd_main()
    sys.exit(rc)
