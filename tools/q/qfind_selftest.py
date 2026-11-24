#!/usr/bin/env python3
"""Selftest for q's front door (S108 lane FD): `q find`, `q multi`, the sizing rule (qcap), the no-dead-end fallbacks, `--in`, the unknown-verb route,
and the qtools content seeding. Every case goes through the REAL CLI (`py -I tools/q/q.py ...` as a subprocess), against a fixture git repo in a TEMP
folder (C++ answered by text (qsym), python / ps1 tools, a docs page for fact) plus three kinds that need the real game / SDK indexes.

    py -I tools/q/qfind_selftest.py [--q PATH_TO_q.py]     (--q: run the same cases against another q.py, e.g. the pre-FD tree: they must FAIL there)

0 game runs.
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
    'src/zq/zq_alpha.h': '''#pragma once
#define REXCVAR_DEFINE_BOOL(n, d, c, h) inline bool FLAGS_##n = d
REXCVAR_DEFINE_BOOL(zq_cvar_flag, false, "zq", "a fixture cvar");
namespace zq {
inline int ZqAlpha(int v) { return v + 1; }
}  // namespace zq
''',
    'src/zq/zq_use.cpp': '''#include "zq_alpha.h"
#include <cstdio>
#include <cstdlib>
namespace zq {
int ZqUse() {
  std::printf("[zq][probe] value %d\\n", ZqAlpha(2));
  return std::getenv("ZQ_ENV_KNOB") != nullptr;
}
}  // namespace zq
''',
    'tools/zq/zq_tool.py': '''import os
import subprocess
import zq_mod


def zq_helper(x):
    return os.environ.get('ZQ_ENV_KNOB'), x, zq_mod.ZQ_CONST


def run():
    subprocess.run(['pwsh', '-File', 'tools/zq/run_zq.ps1'])
    return zq_helper(1)
''',
    'tools/zq/zq_other.py': '''from zq_tool import zq_helper


def again():
    return zq_helper(2), zq_helper(3)
''',
    'tools/zq/zq_mod.py': 'ZQ_CONST = 3\n',
    'tools/zq/run_zq.ps1': 'function Invoke-ZqThing { param($a) Write-Host $a }\nInvoke-ZqThing 1\n',
    'tools/zq/paths.py': "OUT = 'out\\\\zq\\\\frozen_store'\n",
    # S108 FD review (the coordinator's typo rule): a word near a verb that IS a name here: a file stem (qsave ~ save), a python def (vcalls ~ vcall)
    'tools/zq/qsave.py': 'X = 1\n',
    'tools/zq/zq_verbs.py': 'def vcalls(x):\n    return x\n',
    # a word near a verb that appears only as TEXT (a comment): still a typo
    'tools/zq/zq_note.py': '# the word decmpx sits in this comment only\n',
    'docs/systems/zq.md': '# ZQ system\n\n## Probe\n\nThe zq probe page: a planted doc word, quuxplanted, next to ZqAlpha.\n',
}
for _i in range(35):
    FIX[f'tools/zq/many/m{_i}.py'] = f'zqmany = {_i}\n'
for _i in range(8):
    FIX[f'tools/zq/small/s{_i}.py'] = f'zqsmall = {_i}\n'
# review (fact total under --in): 60 short system pages outrank 5 long format pages for `zqfacty`; the format pages are past the first 50 hits
for _i in range(60):
    FIX[f'docs/systems/zqf{_i}.md'] = f'# zqfacty page {_i}\n\nzqfacty short.\n'
for _i in range(5):
    FIX[f'docs/formats/zqfmt{_i}.md'] = f'# Format {_i}\n\n' + 'lorem ' * 150 + 'zqfacty ' + 'ipsum ' * 150 + '\n'


def write_fixture(root):
    for rel, txt in FIX.items():
        p = os.path.join(root, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, 'w', encoding='utf-8', newline='\n') as f:
            f.write(txt)
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
            print('      ' + detail.strip().replace('\n', '\n      ')[:1500])


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--q', default=os.path.join(HERE, 'q.py'))
    ap.add_argument('--keep', action='store_true')
    a = ap.parse_args(argv)
    Q = os.path.abspath(a.q)
    qdir = os.path.dirname(Q)
    env = dict(os.environ, WWE_Q_TIMEOUT='300', PYTHONIOENCODING='utf-8')
    root = tempfile.mkdtemp(prefix='qfind_selftest_')
    db = os.path.join(root, 'out', 'harness', 'index', 'ours.db')
    fx = ['--root', root, '--db', db]

    def q(*args, timeout=180):
        t0 = time.time()
        r = subprocess.run([sys.executable, '-I', Q] + list(args), capture_output=True, text=True, encoding='utf-8', errors='replace', env=env, timeout=timeout)
        return r.returncode, r.stdout, r.stderr, time.time() - t0

    try:
        write_fixture(root)
        # ---- one case per kind
        kinds = [
            (['ZqAlpha'], ['C++ symbol: 1 match', 'zq::ZqAlpha', 'src/zq/zq_alpha.h:5', 'zq::ZqUse'], 'a C++ symbol: definition, refs, its caller'),
            (['zq_helper'], ['python / ps1 name', 'tools/zq/zq_tool.py:6', 'def zq_helper'], 'a python name: its def line and uses'),
            (['Invoke-ZqThing'], ['python / ps1 name', 'tools/zq/run_zq.ps1:1'], 'a ps1 function'),
            (['zq_mod'], ['file: tools/zq/zq_mod.py', 'imported by', 'tools/zq/zq_tool.py:3'], 'a file stem: the file and who imports it'),
            (['run_zq.ps1'], ['file: tools/zq/run_zq.ps1', 'invoked by', 'tools/zq/zq_tool.py:11'], 'a script name: who invokes it'),
            (['out/zq/frozen_store'], ['path literal', 'tools/zq/paths.py:1'], 'a path literal written with the other separator'),
            (['zq_cvar_flag'], ['cvar zq_cvar_flag', 'src/zq/zq_alpha.h:3'], 'a cvar define'),
            (['ZQ_ENV_KNOB'], ['env var ZQ_ENV_KNOB', 'src/zq/zq_use.cpp:7', 'tools/zq/zq_tool.py:7'], 'an env var: read in C++ and python'),
            (['[zq][probe] value'], ['log line: printed by 1', 'src/zq/zq_use.cpp:6'], 'a log line: its printf site'),
            (['quuxplanted'], ['docs: 1 lines in 1 files', 'docs/systems/zq.md:5', 'fact:', 'docs/systems/zq.md'], 'a docs word and its fact section'),
            (['tools/zq/zq_mod.py:1-1'], ['== tools/zq/zq_mod.py:1-1', 'ZQ_CONST = 3'], 'FILE:N-M = the lines'),
        ]
        for args, wants, what in kinds:
            rc, so, se, dt = q('find', *args, *fx)
            miss = [w for w in wants if w not in so]
            check(rc == 0 and not miss, f'find {args[0]}: {what} ({dt:.2f} s)', f'rc {rc}; missing {miss}\n{so}\n{se}')
        # the real indexes (game, db names, SDK): the checkout's own root
        for args, wants, what in ((['sub_82CB1F50'], ['game function sub_82CB1F50', 'gpu_wait_poll_step', 'next: q fn'], 'a game address: function, db name, callers'),
                                  (['gpu_wait_poll_step'], ['game function sub_82CB1F50'], 'a db name -> the game function'),
                                  (['RunQuitFlushes'], ['sdk (index as last built)'], 'an SDK name')):
            rc, so, se, dt = q('find', *args)
            miss = [w for w in wants if w not in so]
            check(rc == 0 and not miss, f'find {args[0]} (real indexes): {what} ({dt:.2f} s)', f'rc {rc}; missing {miss}\n{so[:1500]}\n{se[-500:]}')
        # ---- no hit
        rc, so, se, dt = q('find', 'zqnothingatallxyz', *fx)
        check(rc == 1 and 'no hit as:' in so and 'nothing anywhere' in so, 'find with no hit anywhere: exit 1, the `no hit as:` line names every kind tried', f'rc {rc}\n{so}\n{se}')
        # ---- sizing
        rc, so, se, dt = q('find', 'zqmany', *fx)
        check(rc == 0 and 'text in code: 35 lines in 35 files' in so and 'showing 6 of 35 files; all: q find zqmany --all, or narrow: add --in PATH' in so,
              'trim: a cut list says its total and the exact call for the rest', so)
        rc, so, se, dt = q('find', 'zqsmall', *fx)
        check(rc == 0 and 'text in code: 8 lines in 8 files' in so and 'showing' not in so and so.count('(1)  zqsmall') == 8,
              'a small list (8, cap 6) is never cut', so)
        rc, so, se, dt = q('find', 'zqmany', '--all', *fx)
        check(rc == 0 and so.count('(1)  zqmany') == 35 and 'showing' not in so, '--all shows every file', so[-600:])
        rc, so, se, dt = q('find', 'zqmany', '--in', 'many/m7.py', *fx)
        check(rc == 0 and 'text in code: 1 lines in 1 files' in so and '34 more lines outside --in' in so, '--in narrows and says how much it left out', so)
        rc, so, se, dt = q('find', 'zqmany', '--json', *fx)
        try:
            js = json.loads(so)
            nf = len(js['answers'][0]['kinds']['text in code']['files'])
        except (ValueError, KeyError, IndexError, TypeError):
            nf = None
        check(rc == 0 and nf == 35, f'--json is never cut (35 files in the json; got {nf})', so[:300])
        # review: --in written as an absolute path or with ./ narrows exactly like the repo-relative form (it matched nothing before)
        for spec in (os.path.join(root, 'tools', 'zq', 'many', 'm7.py'), './tools/zq/many/m7.py', '.\\tools\\zq\\many\\m7.py'):
            rc, so, se, dt = q('find', 'zqmany', '--in', spec, *fx)
            check(rc == 0 and 'text in code: 1 lines in 1 files' in so and '34 more lines outside --in' in so, f'--in {spec!r} = --in tools/zq/many/m7.py', so + se)
        # review: an --in that leaves out EVERY line says so (never a bare "no hit")
        rc, so, se, dt = q('find', 'zqmany', '--in', 'zq_no_such_dir', *fx)
        check('35 code lines' in so and 'outside --in' in so, '--in that leaves out every line names what it left out', so + se)
        # review: the fact total under --in counts every matching section in PATH, not those of the first 50 hits
        rc, so, se, dt = q('find', 'zqfacty', '--in', 'docs/formats', *fx)
        check(rc == 0 and 'fact: 5 matching sections' in so, 'fact under --in: all 5 format pages counted (past the first 50 hits)', so + se)
        # ---- several questions, the unknown-verb route, a mistyped verb
        rc, so, se, dt = q('find', 'ZqAlpha', 'zqnothingatallxyz', *fx)
        check(rc == 1 and '== find ZqAlpha' in so and '== find zqnothingatallxyz' in so and 'no hit at all: zqnothingatallxyz' in so,
              'find A B: each answered under its own header; the closing line names the one with no hit', so[-400:])
        rc, so, se, dt = q('ZqAlpha', *fx)
        check(rc == 0 and '== find ZqAlpha' in so and 'not a q verb' in se, 'q X with an unknown first word = q find X (said on stderr)', so[:300] + se)
        rc, so, se, dt = q('decmp', 'ZqAlpha', *fx)
        check(rc == 2 and so.startswith("q: unknown verb 'decmp': did you mean `q decomp"), 'a mistyped verb: `did you mean` on the FIRST stdout line, exit 2', so[:300] + se)
        rc, so, se, dt = q('ours-callers', 'ZqAlpha', *fx)
        check(rc == 2 and so.startswith("q: unknown verb 'ours-callers': did you mean `q ours callers"), 'a verb glued with - (ours-callers): did you mean, exit 2', so[:300] + se)
        # the coordinator's rule: near a verb AND no structural hit = a typo; a structural hit = a question (find's exit, one closing note at most)
        rc, so, se, dt = q('qfact')
        check(rc == 0 and so.startswith('== find qfact') and 'did you mean' not in so and "(also a near-miss of the verb 'fact'" in so,
              'q qfact (a real module): a question, exit 0, no typo line, the closing near-miss note', so[:300] + so[-300:] + se)
        rc, so, se, dt = q('decmp', 'sub_82CB1F50')
        check(rc == 2 and so.startswith("q: unknown verb 'decmp': did you mean `q decomp"), 'q decmp sub_X (real tree): the typo line FIRST, exit 2', so[:300] + se)
        rc, so, se, dt = q('qsave', *fx)
        check(rc == 0 and so.startswith('== find qsave') and 'file: tools/zq/qsave.py' in so and "(also a near-miss of the verb 'save'" in so,
              'a near-verb word that is a file stem: a question, exit 0, closing note', so + se)
        rc, so, se, dt = q('vcalls', *fx)
        check(rc == 0 and so.startswith('== find vcalls') and 'tools/zq/zq_verbs.py:1' in so and "(also a near-miss of the verb 'vcall'" in so,
              'a near-verb word that is a python symbol: a question, exit 0, closing note', so + se)
        rc, so, se, dt = q('decmpx', *fx)
        check(rc == 2 and so.startswith("q: unknown verb 'decmpx': did you mean `q decomp") and 'text in code' in so,
              'a near-verb word found only as TEXT: still the typo line first, exit 2', so + se)
        # ---- multi
        rc, so, se, dt = q('multi', f'find ZqAlpha --root {root} --db {db}', f'show nosuch_zq.py:1-2 --root {root}', f'py refs zq_helper --root {root}')
        ok = ('== [1/3] q find ZqAlpha' in so and '-- [1/3] exit 0' in so and '== [2/3] q show nosuch_zq.py' in so and '-- [2/3] exit 2' in so
              and '-- [3/3] exit 0' in so and 'failed: #2 (exit 2)' in so and so.index('no such file') < so.index('-- [2/3]'))
        check(rc == 2 and ok, 'multi: three questions, the failing one keeps its exit and its error inside its own block; the others answered; exit 2', so + se)
        rc, so, se, dt = q('multi', '--json', f'ours callers ZqAlpha --json --root {root} --db {db}', f'show nosuch_zq.py:1-2 --root {root}')
        try:
            js = json.loads(so)
            ok = (isinstance(js, list) and len(js) == 2 and js[0]['exit'] == 0 and isinstance(js[0]['output'], dict) and js[1]['exit'] == 2
                  and 'no such file' in js[1]['stderr'] and 'seconds' in js[0])
        except (ValueError, KeyError, TypeError):
            ok = False
        check(rc == 2 and ok, 'multi --json: a list of {question, exit, seconds, output (parsed --json), stderr}', so[:600] + se)
        # review: a question that does not split (an unclosed quote) is its own exit 2; the next one is still answered; `py -I tools\q\q.py` is dropped
        rc, so, se, dt = q('multi', "find it's", f'py -I tools\\q\\q.py find ZqAlpha --root {root} --db {db}')
        check(rc == 2 and '-- [1/2] exit 2' in so and '-- [2/2] exit 0' in so and 'zq::ZqAlpha' in so, 'multi: an unclosed quote fails only its question; a `py -I ...q.py` prefix is dropped', so + se)
        # review: find --root FIX inside multi must not leave the fact library pointed at FIX for the next question
        rc, so, se, dt = q('multi', f'find quuxplanted --root {root} --db {db}', 'fact quuxplanted')
        second = so.split('== [2/2]', 1)[-1]
        check('zq.md' in so.split('== [2/2]', 1)[0] and 'zq.md' not in second, 'multi: find --root does not change what the next `fact` searches', so + se)
        # ---- no dead ends
        rc, so, se, dt = q('py', 'refs', 'run_zq', '--root', root)
        check(rc == 0 and 'answered as the script run_zq.ps1' in so and 'tools/zq/zq_tool.py:11' in so, 'py refs <a script stem> answers as the script (who invokes it)', so + se)
        rc, so, se, dt = q('ours', 'refs', 'quuxplanted', *fx)
        check(rc == 2 and 'found elsewhere:' in so and 'docs 1 lines' in so and 'q find quuxplanted' in so, 'ours refs of no symbol: exit 2 AND what the other probes found', so + se)
        rc, so, se, dt = q('py', 'refs', 'zq_helper', '--in', 'zq_other', '--root', root)
        check(rc == 0 and 'tools/zq/zq_other.py:5' in so and '\n  tools/zq/zq_tool.py' not in so and 'more outside it' in so, 'py refs --in narrows to one file', so + se)
        rc, so, se, dt = q('py', 'refs', 'zq_helper', '--in', os.path.join(root, 'tools', 'zq', 'zq_other.py'), '--root', root)
        check(rc == 0 and 'tools/zq/zq_other.py:5' in so and '\n  tools/zq/zq_tool.py' not in so, 'py refs --in <an absolute path> narrows like the relative one', so + se)
        # ---- qtools content seeding (a fresh tree reuses the parses of the same text): in-process, the tree under test
        sys.path.insert(0, qdir)
        import qtools
        sa, sb = os.path.join(root, 'seed_a'), os.path.join(root, 'seed_b')
        for d in (sa, sb):
            for i in range(60):
                p = os.path.join(d, 'tools', f'f{i}.py')
                os.makedirs(os.path.dirname(p), exist_ok=True)
                with open(p, 'w', encoding='utf-8', newline='\n') as f:
                    f.write(f'def seeded_{i}(x):\n    return x + {i}\n')
        try:
            qtools.load(sa)
            before = dict(qtools.STATS)
            idx = qtools.load(sb, seed=sa)
            got = {k: qtools.STATS[k] - before[k] for k in before}
            ok = got == {'parsed': 0, 'reused': 60} and any(d[1] == 'seeded_7' for d in idx['tools/f7.py']['defs'])
        except (TypeError, AttributeError, KeyError) as e:
            ok, got = False, repr(e)
        check(ok, f'qtools: a fresh tree with the same 60 files reuses the seed tree\'s parses by content (parsed / reused: {got})')
    finally:
        if not a.keep:
            shutil.rmtree(root, ignore_errors=True)
        else:
            print(f'kept {root}')
    if FAILS:
        print(f'qfind_selftest: FAIL ({len(FAILS)} checks)')
        return 1
    print('qfind_selftest: PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())
