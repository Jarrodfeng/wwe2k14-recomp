#!/usr/bin/env python3
"""Selftest of tools\\handback\\check.py (S108 rewrite): scratch git repos in a temp folder, stub lints planted in them, no compile, no game.
  py -I tools\\handback\\check_selftest.py            (HBK_DIR=<folder holding check.py> runs the CLI cases against another copy, e.g. the one before S108)
One planted fault per diff-reading arm (each MUST give its verdict), every lint row runs on a docs-only diff, every `--list` row has a verdict, one clean case, and the git mv / intended deletion case that must NOT fail (the old a.deleted failed both)."""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import types

HERE = os.path.dirname(os.path.abspath(__file__))
HBK_DIR = os.environ.get('HBK_DIR', HERE)
sys.path.insert(0, HERE)
import check as hb   # noqa: E402

FAILS = []
ENV = dict(os.environ, GIT_AUTHOR_NAME='t', GIT_AUTHOR_EMAIL='t@t', GIT_COMMITTER_NAME='t', GIT_COMMITTER_EMAIL='t@t')
TMP = None
_N = [0]


def ok(cond, text):
    print(('ok    ' if cond else 'FAIL  ') + text, flush=True)
    if not cond:
        FAILS.append(text)


def git(root, *a):
    r = subprocess.run(['git', '-C', root] + list(a), env=ENV, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(r.stderr)
    return r.stdout.strip()


def put(root, rel, text):
    p = os.path.join(root, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, 'wb') as f:
        f.write(text if isinstance(text, bytes) else text.encode('utf-8'))


def commit(r, msg):
    git(r, 'add', '-A')
    git(r, 'commit', '-qm', msg)
    return git(r, 'rev-parse', 'HEAD')


PASS_LINT = 'import sys\nprint("stub lint: 0 violation(s)")\n'
FAIL_LINT = 'import sys\nprint("VIOLATION stub: planted")\nsys.exit(1)\n'
BLOCK_LINT = 'import sys\nprint("BLOCKED: cannot judge")\nsys.exit(2)\n'
CRASH_LINT = 'raise RuntimeError("lint bug")\n'


def repo(name):
    _N[0] += 1
    r = os.path.join(TMP, '%02d_%s' % (_N[0], name))
    os.makedirs(r)
    git(r, 'init', '-q', '-b', 'main')
    return r


SCRIPTS = {row.name: row.script for row in hb.LINT_ROWS}   # row name -> the lint script's path


def new_repo(name, override=None, files=None):
    r = repo(name)
    for rn, rel in SCRIPTS.items():
        put(r, rel, (override or {}).get(rn, PASS_LINT))
    put(r, 'CMakeLists.txt', 'set(WWE2K14_SOURCES\n  src/a.cpp\n)\n')
    put(r, 'src/a.cpp', 'void A() {}\n')
    put(r, 'docs/a.md', '# a\n')
    for rel, t in (files or {}).items():
        put(r, rel, t)
    return r, commit(r, 'c1')


def run(r, base, only=None, all_rows=False):
    res, summ = hb.run_checks(r, base, only, out=lambda *_a: None, all_rows=all_rows)
    return {x.name: x for x in res}, summ


def cli(r, base, *extra):
    """(exit code, output) of `check.py --root R --base B [extra]` from HBK_DIR: the cases that also run against the pre-S108 copy."""
    p = subprocess.run([sys.executable, '-I', os.path.join(HBK_DIR, 'check.py'), '--root', r, '--base', base] + list(extra), capture_output=True, text=True, timeout=600)
    return p.returncode, p.stdout + p.stderr


def handback_json(out):
    """The summary of the CLI's last `HANDBACK {json}` line ({} when there is none)."""
    lines = [ln for ln in out.splitlines() if ln.startswith('HANDBACK ')]
    return json.loads(lines[-1][len('HANDBACK '):]) if lines else {}


# ------------------------------------------------------------------------------------------------------------------------- one planted fault per arm

def t_control():
    r, base = new_repo('ctl', files={'src/x.h': b'// old \x08 line\nint a;\n'})
    put(r, 'src/x.h', b'// old \x08 line\nint a;\nint b;\n')
    v, _ = run(r, base, 'control_chars')
    ok(v['control_chars'].verdict == 'PASS', 'control_chars: a control char on an UNTOUCHED line is not yours')
    put(r, 'src/x.h', b'// old \x08 line\nint a;\nint b; // path out\x08uild\n')
    v, _ = run(r, base, 'control_chars')
    ok(v['control_chars'].verdict == 'FAIL' and 'src/x.h:3' in v['control_chars'].reason, 'control_chars: a 0x08 on an added line FAILS')
    put(r, 'src/x.h', b'// old \x08 line\nint a;\nint b; // lone\rCR\n')
    v, _ = run(r, base, 'control_chars')
    ok(v['control_chars'].verdict == 'FAIL' and 'lone CR' in v['control_chars'].reason, 'control_chars: a CR inside an added line FAILS')


def t_game_text():
    fake = types.ModuleType('game_text_lint')
    fake.MAX_FILE, fake.SKIP_EXT, fake.SELF = 1 << 20, ('.png',), ()
    fake.load_set = lambda: None
    fake.load_allow = lambda: []
    fake.is_local = lambda p: False
    fake.allowed = lambda allow, p, label: False

    class Index:
        def __init__(self, _s):
            pass

        def find(self, text):
            return [(i + 1, 'string.pac#1', 'the game said') for i, ln in enumerate(text.splitlines()) if 'THE GAME SAID THIS' in ln]
    fake.Index = Index
    old = sys.modules.get('game_text_lint')
    sys.modules['game_text_lint'] = fake
    try:
        r, base = new_repo('gt', files={'notes/n.txt': 'one\nTHE GAME SAID THIS (old line)\n'})
        put(r, 'notes/n.txt', 'one\nTHE GAME SAID THIS (old line)\nfine\n')
        v, _ = run(r, base, 'game_text')
        ok(v['game_text'].verdict == 'PASS', 'game_text: game text on an UNTOUCHED line is not yours')
        put(r, 'notes/n.txt', 'one\nTHE GAME SAID THIS (old line)\nTHE GAME SAID THIS too\n')
        v, _ = run(r, base, 'game_text')
        ok(v['game_text'].verdict == 'FAIL' and 'notes/n.txt:3' in v['game_text'].reason, 'game_text: game text on an added line FAILS')
    finally:
        if old is None:
            del sys.modules['game_text_lint']
        else:
            sys.modules['game_text_lint'] = old


def t_new_src():
    r, base = new_repo('ns')
    put(r, 'src/b.cpp', 'void B() {}\n')
    v, _ = run(r, base, 'new_src')
    ok(v['new_src'].verdict == 'FAIL' and 'src/b.cpp' in v['new_src'].reason, 'new_src: a new src unit no build list names FAILS')
    put(r, 'CMakeLists.txt', 'set(WWE2K14_SOURCES\n  src/a.cpp\n  src/b.cpp\n)\n')
    v, _ = run(r, base, 'new_src')
    ok(v['new_src'].verdict == 'PASS', 'new_src: the same unit named in CMakeLists.txt PASSES (%s)' % v['new_src'].reason)


def t_log_readers():
    fmt_old = '  std::snprintf(e, sizeof e, "[scene][maps][e2] plane made %llu of %llu seen", made, seen);\n'
    fmt_new = '  std::snprintf(e, sizeof e, "[scene][maps][e2] plane made %llu (draws %u)", made, draws);\n'
    reader = ('import os, sys\n# reads [scene][maps][e2]\nif "--selftest" in sys.argv:\n    t = open(os.path.join(os.getcwd(), "src", "m.cpp")).read()\n'
              '    ok = "of %llu seen" in t\n    print("selftest PASS" if ok else "selftest FAIL: the e2 shape changed"); sys.exit(0 if ok else 1)\n')
    r, base = new_repo('lr', files={'src/m.cpp': 'void A() {\n' + fmt_old + '}\n', 'tools/scene/e2_reader.py': reader})
    put(r, 'src/m.cpp', 'void A() {\n' + fmt_new + '}\n')
    v, _ = run(r, base, 'log_readers')
    ok(v['log_readers'].verdict == 'FAIL' and 'e2_reader.py' in v['log_readers'].reason, 'log_readers: a changed census format whose reader selftest breaks FAILS')
    put(r, 'tools/scene/e2_reader.py', reader.replace('"of %llu seen" in t', '"draws %u" in t'))
    v, _ = run(r, base, 'log_readers')
    ok(v['log_readers'].verdict == 'PASS', 'log_readers: the reader updated with the format PASSES (%s)' % v['log_readers'].reason)


def t_lint_runner():
    r, base = new_repo('lint', override={'dev_cvar': FAIL_LINT, 'getenv': CRASH_LINT, 'rule_register': BLOCK_LINT})
    put(r, 'src/a.cpp', 'void A() { getenv("X"); /* SHADER-INPUT */ }\n')
    v, s = run(r, base, 'dev_cvar,getenv,rule_register,switch_defined')
    ok(v['dev_cvar'].verdict == 'FAIL' and 'planted' in v['dev_cvar'].reason, 'lint runner: a lint exiting 1 is a FAIL with its line')
    ok(v['getenv'].verdict == 'BLOCKED' and 'crashed' in v['getenv'].reason, 'lint runner: a lint that crashes is BLOCKED, never PASS')
    ok(v['rule_register'].verdict == 'BLOCKED', 'lint runner: a lint exiting 2 with BLOCKED is BLOCKED')
    ok(v['switch_defined'].verdict == 'PASS' and s['exit'] == 1, 'lint runner: a passing lint PASSES; a FAIL gives exit 1 (%d)' % s['exit'])
    v, s = run(r, base, 'getenv,rule_register')
    ok(s['exit'] == 2, 'lint runner: BLOCKED without FAIL gives exit 2 (%d)' % s['exit'])


def t_runs_all():
    """No triggers: on a docs-only diff every lint row still RUNS (each lint stub is planted to FAIL, so a FAIL is the proof it ran); through the real CLI (HBK_DIR's
    check.py, its HANDBACK line) a `git mv` of a docs file runs them too, and only an `--only` run says partial."""
    r, base = new_repo('runs_all', override={row.name: FAIL_LINT for row in hb.LINT_ROWS})
    put(r, 'docs/a.md', '# a\nmore\n')
    v, s = run(r, base)
    gated = set(hb.check_gates.GATES)
    not_run = [row.name for row in hb.LINT_ROWS if row.name not in gated and v[row.name].verdict != 'FAIL']
    ok(not not_run, 'runs all: a docs-only diff still runs every ungated lint row (%d rows; not run: %s)' % (len(hb.LINT_ROWS) - len(gated), not_run or 'none'))
    ok({n for n, x in v.items() if x.verdict == 'SKIP'} == gated and s['counts']['SKIP'] == len(gated),
       'runs all: on a docs-only diff exactly the gated rows are SKIP (%s)' % sorted(n for n, x in v.items() if x.verdict == 'SKIP'))
    v, s = run(r, base, all_rows=True)
    ok(not [n for n in gated if n in SCRIPTS and v[n].verdict != 'FAIL'] and not [n for n in gated if n.startswith('cd_') and v[n].verdict == 'SKIP'],
       'runs all: all_rows runs the gated rows too')
    v, _ = run(r, base, 'aniso')
    ok(v['aniso'].verdict == 'FAIL' and v['mask'].verdict == 'SKIP', 'runs all: --only runs just the named row (the others are SKIP, by --only)')
    r, base = new_repo('runs_all_cli', override={row.name: FAIL_LINT for row in hb.LINT_ROWS})
    git(r, 'mv', 'docs/a.md', 'docs/b.md')
    rc, out = cli(r, base)
    j = handback_json(out)
    rows = j.get('rows', {})
    not_run = [row.name for row in hb.LINT_ROWS if row.name not in hb.check_gates.GATES and rows.get(row.name) != 'FAIL']
    ok(rc == 1 and not not_run and j.get('counts', {}).get('SKIP') == len(hb.check_gates.GATES) and j.get('partial') is False,
       'runs all (CLI): a `git mv` docs diff runs every ungated lint row, skips the gated ones, exit 1, not partial (exit %d; not run: %s; partial %r)' % (rc, not_run or 'none', j.get('partial')))
    rc, out = cli(r, base, '--all-rows')
    ok(handback_json(out).get('counts', {}).get('SKIP') == 0, 'runs all (CLI): --all-rows runs the gated rows too')
    rc, out = cli(r, base, '--only', 'aniso')
    j = handback_json(out)
    ok(j.get('partial') is True and j.get('rows', {}).get('mask') == 'SKIP', 'runs all (CLI): an --only run says partial in its HANDBACK line (partial %r)' % j.get('partial'))


def t_gates():
    """S111 CHECKFAST: each gated row's SKIP decision with a planted twin: a diff that touches the row's input MUST run it (the stub lint FAILs, so a FAIL = it ran), a diff
    that holds none of its inputs is SKIP with the reason. The twins are first seen to fail on the old check.py (every row ran: the SKIP cases red) and on a gate that never fires."""
    all_gated = sorted(hb.check_gates.GATES)
    gated = [n for n in all_gated if n in SCRIPTS]   # the lint rows (the cd_ rows are t_cdfields')
    fail_all = {n: FAIL_LINT for n in gated}
    enum_h = 'enum FxKind : unsigned char { kFxA = 1, kFxB = 2 };\ninline constexpr unsigned char kFxLast = kFxB;\n'
    base_files = {'src/renderer_game/frame_tick.h': '// ft\n#include "../renderer_scene/live_scene.h"\n#include "../renderer_adapter/tail_release.h"\n'
                                                    '#if !defined(WWE2K14_FRAME_TICK_CORE_ONLY)\n#include "../frame_stats.h"\n#endif\n',
                  'tools/renderer/frame_tick_unit.cpp': '#define WWE2K14_FRAME_TICK_CORE_ONLY 1\n#include "../../src/renderer_game/frame_tick.h"\n',
                  'src/renderer_scene/live_scene.h': '#include "record_io.h"\nstruct Latched { int a; };\n', 'src/renderer_scene/record_io.h': 'struct SceneTables {};\n',
                  'src/renderer_adapter/tail_release.h': 'inline int FlushIfPending() { return 0; }\n', 'src/frame_stats.h': '// game part only\n',
                  'src/half2_hooks.h': '// h\n#include "half2_hooks/present.inc"  // split: present\n', 'src/half2_hooks/present.inc': '// part\n',
                  'src/e.h': enum_h, 'src/plain.cpp': 'void P() {}\n',
                  'src/renderer_adapter/reader/p.inc': '// p\n',
                  'tools/renderer/pseed_catalog.cpp': '#include "../../src/renderer/pipeline_store.h"\n', 'src/renderer/pipeline_store.h': '// store\n'}

    def verdicts(name, mutate):
        r, base = new_repo(name, override=fail_all, files=base_files)
        mutate(r)
        v, _ = run(r, base)
        return v

    def ran(v, rows):
        return [n for n in rows if v[n].verdict == 'FAIL']

    def skipped(v, rows):
        return [n for n in rows if v[n].verdict == 'SKIP']
    coarse = ['cvar_link', 'one_definition', 'cvar_shard', 'build_list']
    v = verdicts('g_docs', lambda r: put(r, 'docs/a.md', '# a\nmore\n'))
    ok(skipped(v, all_gated) == all_gated and all('no input' in v[n].reason for n in all_gated), 'gates: a docs-only diff SKIPs every gated row, with the reason (%s)' % [n for n in all_gated if v[n].verdict != 'SKIP'])
    v = verdicts('g_plain', lambda r: put(r, 'src/plain.cpp', 'void P() { int x = 1; }\n'))
    ok(ran(v, coarse) == coarse and skipped(v, ['frame_tick', 'kind_last']) == ['frame_tick', 'kind_last'],
       'gates: an edit of a src .cpp with no enum / kXxLast / boundary call runs the four whole-tree rows and SKIPs frame_tick and kind_last')
    for rel in ('src/renderer_game/frame_tick.h', 'src/half2_hooks.h', 'src/renderer_adapter/reader/p.inc'):
        v = verdicts('g_ft', lambda r, rel=rel: put(r, rel, '// changed\n'))
        ok(ran(v, ['frame_tick']) == ['frame_tick'], 'gates: a change of %s RUNS frame_tick' % rel)
    v = verdicts('g_ft_call', lambda r: put(r, 'src/plain.cpp', 'void P() { rdr_scene::live::EndFrame(1, 2); }\n'))
    ok(ran(v, ['frame_tick']) == ['frame_tick'], 'gates: a src file that ADDS a scene-writer boundary call RUNS frame_tick')
    # S111 CHECKFAST review: the unit COMPILES frame_tick.h with every header it includes (the scene writer's live_scene.h -> record_io.h, tail_release.h): an edit of
    # one of them with no boundary-call name in it (a struct field, the tail's flush) changes what the unit builds and runs
    for rel in ('src/renderer_scene/live_scene.h', 'src/renderer_scene/record_io.h', 'src/renderer_adapter/tail_release.h', 'src/half2_hooks/present.inc'):
        v = verdicts('g_ft_inc', lambda r, rel=rel: put(r, rel, '// changed: no boundary name\n'))
        ok(ran(v, ['frame_tick']) == ['frame_tick'], 'gates: a change of %s (in the unit\'s include closure / a split part of a linted file) RUNS frame_tick' % rel)
    v = verdicts('g_kl_enum', lambda r: put(r, 'src/e.h', enum_h.replace('kFxB = 2', 'kFxB = 2, kFxC = 3')))
    ok(ran(v, ['kind_last']) == ['kind_last'], 'gates: an added enum kind in a src header RUNS kind_last')
    v = verdicts('g_kl_del', lambda r: os.remove(os.path.join(r, 'src', 'e.h')))
    ok(ran(v, ['kind_last']) == ['kind_last'], 'gates: a deleted src header RUNS kind_last')
    v = verdicts('g_kl_lint', lambda r: put(r, 'tools/lint/kind_last_lint.py', FAIL_LINT + '# x\n'))
    ok(ran(v, ['kind_last']) == ['kind_last'], 'gates: a change of the lint itself RUNS kind_last')
    for rel in ('db/shaders/shader_list.txt', 'src/renderer/shaders/spv/rdr_flash_vert.h', 'tools/renderer/pseed_catalog.cpp', 'src/renderer/pipeline_store.h'):
        v = verdicts('g_ps', lambda r, rel=rel: put(r, rel, '// changed\n'))
        ok(ran(v, ['pipeline_seed']) == ['pipeline_seed'], 'gates: a change of %s (the seed list, the compiled SPIR-V, the catalog printer or a header it compiles) RUNS pipeline_seed' % rel)
    v = verdicts('g_ps_no', lambda r: put(r, 'src/plain.cpp', 'void P() { int y = 2; }\n'))
    ok(skipped(v, ['pipeline_seed']) == ['pipeline_seed'], 'gates: a src .cpp outside the catalog\'s inputs SKIPs pipeline_seed')
    v = verdicts('g_self', lambda r: put(r, 'tools/handback/check.py', '# a copy of the runner changed\n'))
    ok(ran(v, gated) == gated, 'gates: a change of check.py runs every gated row')
    r, base = new_repo('g_only', override=fail_all, files=base_files)
    put(r, 'docs/a.md', '# a\nmore\n')
    v, _ = run(r, base, 'kind_last')
    ok(v['kind_last'].verdict == 'FAIL', 'gates: --only naming a gated row runs it whatever the diff')


def t_cdfields():
    """S111 CDCHECKALL: the cd_levels / cd_targetfmt / cd_draworder rows with planted extractor rows (check_cdfields.COLLECT swapped): a NEW failing row (not at the base)
    FAILS, the same row at the base passes, an UNSETTLED one passes, a diff with none of the extractor's inputs is SKIP, and the gate runs it on each input."""
    import check_cdfields as cdf
    saved = dict(cdf.COLLECT)
    cases = (('levels', 'cd_levels', 'src/plain.cpp', 'GAME-ONLY'), ('targetfmt', 'cd_targetfmt', 'src/renderer_scene/live_crowd.cpp', 'DIFF'),
             ('draworder', 'cd_draworder', 'src/renderer_scene/front_end.cpp', 'DIFF'))
    try:
        for name, row, touch, verdict in cases:
            def plant(now_v, base_v):
                def collect(ref):
                    v = base_v if ref else now_v
                    return [{'key': ('k', 1), 'verdict': v, 'text': '%s planted' % v}, {'key': ('other', 2), 'verdict': 'OK', 'text': 'ok'}]
                return collect

            def verdict_of(now_v, base_v, rel):
                cdf.COLLECT[name] = plant(now_v, base_v)
                r, base = new_repo('cdf_' + name, files={rel: 'int a;\n'})
                put(r, rel, 'int b;\n')
                v, _ = run(r, base)
                return v[row]
            x = verdict_of(verdict, 'OK', touch)
            ok(x.verdict == 'FAIL' and 'planted' in x.reason, '%s: a NEW %s (not at the base) FAILS (%s: %s)' % (row, verdict, x.verdict, x.reason[:80]))
            x = verdict_of(verdict, verdict, touch)
            ok(x.verdict == 'PASS' and 'already at the base' in x.reason, '%s: the same %s at the base passes (%s: %s)' % (row, verdict, x.verdict, x.reason[:80]))
            x = verdict_of('UNSETTLED', 'OK', touch)
            ok(x.verdict == 'PASS', '%s: an UNSETTLED row passes (%s)' % (row, x.verdict))
            cdf.COLLECT[name] = plant(verdict, 'OK')
            r, base = new_repo('cdf_docs_' + name)
            put(r, 'docs/a.md', '# a\nmore\n')
            v, _ = run(r, base)
            ok(v[row].verdict == 'SKIP' and 'no input' in v[row].reason, '%s: a docs-only diff skips it with the reason (%s)' % (row, v[row].verdict))
    finally:
        cdf.COLLECT.update(saved)
    r, base = new_repo('cdf_gate', files={'src/plain.cpp': 'int a;\n'})
    put(r, 'src/plain.cpp', 'int b;\n')
    ch = hb.Change(r, base)
    ok(all(hb.check_gates.decide(ch, n)[0] for n in ('cd_levels',)) and not hb.check_gates.decide(ch, 'cd_targetfmt')[0] and not hb.check_gates.decide(ch, 'cd_draworder')[0],
       'cdf gates: a src .cpp that is none of the targetfmt / draworder files runs levels only')
    for rel, rows_run in (('src/renderer_scene/see_through_order.h', ('cd_levels', 'cd_draworder')), ('src/renderer/shaders/fx/yLiquidMap.frag', ('cd_targetfmt',)),
                          ('docs/systems/render_categories.md', ('cd_draworder',)), ('tools/q/qlevels.py', ('cd_levels',)), ('src/renderer_host/scene_host.cpp', ('cd_levels', 'cd_targetfmt'))):
        r, base = new_repo('cdf_gate2', files={rel: 'x\n'})
        put(r, rel, 'y\n')
        ch = hb.Change(r, base)
        got = tuple(n for n in ('cd_levels', 'cd_targetfmt', 'cd_draworder') if hb.check_gates.decide(ch, n)[0])
        ok(got == rows_run, 'cdf gates: %s runs %s (ran %s)' % (rel, rows_run, got))


def t_list_complete():
    """Every row `--list` prints appears in a plain run's results as PASS / FAIL / BLOCKED (never missing, never SKIP)."""
    p = subprocess.run([sys.executable, '-I', os.path.join(HBK_DIR, 'check.py'), '--list'], capture_output=True, text=True, timeout=60)
    listed = [ln.split()[0] for ln in p.stdout.splitlines()[1:] if ln.strip()]
    r, base = new_repo('list_all')
    put(r, 'docs/a.md', '# a\nmore\n')
    v, _ = run(r, base)
    bad = [n for n in listed if n not in v or v[n].verdict not in (('PASS', 'FAIL', 'BLOCKED', 'SKIP') if n in hb.check_gates.GATES else ('PASS', 'FAIL', 'BLOCKED'))]
    ok(len(listed) == len(hb.ROWS) and not bad, 'list: all %d rows of --list have a verdict in a plain run (a gated row may be SKIP; bad: %s)' % (len(listed), bad or 'none'))


def t_verdicts():
    """A lint that exits 1 with findings AND a `BLOCKED` line (cvar_link_lint: a finding plus a unit it could not preprocess) is a FAIL, not BLOCKED; a row that raises
    SystemExit (game_text_lint.load_allow on a bad allow row) is that row's BLOCKED, never the end of the run."""
    lint = 'import sys\nprint("BLOCKED: src/u.cpp [ON]: cannot preprocess")\nprint("error: wwe2k14_x is defined in 2 units")\nsys.exit(1)\n'
    r, base = new_repo('vd', override={'dev_cvar': lint})
    put(r, 'src/a.cpp', 'void A() { int x; }\n')
    v, s = run(r, base, 'dev_cvar')
    ok(v['dev_cvar'].verdict == 'FAIL' and s['exit'] == 1, 'verdict: exit 1 with a finding and a BLOCKED line is a FAIL (%s, exit %d)' % (v['dev_cvar'].verdict, s['exit']))
    fake = types.ModuleType('game_text_lint')
    fake.load_set = lambda: None
    fake.Index = lambda _s: None

    def bad_allow():
        raise SystemExit('game_text_allow.txt: a row needs `path | id-or-* | reason`')
    fake.load_allow = bad_allow
    old = sys.modules.get('game_text_lint')
    sys.modules['game_text_lint'] = fake
    try:
        r, base = new_repo('vd_exit')
        put(r, 'notes/n.txt', 'x\n')
        try:
            v, s = run(r, base, 'game_text,control_chars')
            ok(v['game_text'].verdict == 'BLOCKED' and v['control_chars'].verdict == 'PASS' and s['exit'] == 2,
               'verdict: a row raising SystemExit is BLOCKED and the run still ends with its summary (%s, exit %d)' % (v['game_text'].verdict, s['exit']))
        except SystemExit as e:
            ok(False, 'verdict: a row raising SystemExit ended the whole run (%s)' % e)
    finally:
        if old is None:
            del sys.modules['game_text_lint']
        else:
            sys.modules['game_text_lint'] = old


def t_clean():
    r, base = new_repo('clean')
    put(r, 'docs/a.md', '# a\nmore\n')
    v, s = run(r, base)
    ok(s['exit'] == 0 and not s['fail'] and not s['blocked'], 'clean: a docs-only change gives exit 0 over every row (%s)' % s['rows'])


def t_rename_delete():
    """A git mv and an intended deletion are not findings (the old a.deleted FAILED both: this case is seen red against the pre-S108 copy via HBK_DIR)."""
    r, base = new_repo('mv', files={'docs/old.md': '# old\n', 'docs/gone.md': '# gone\n', 'src/m.h': '#pragma once\nint m;\n'})
    git(r, 'mv', 'docs/old.md', 'docs/new.md')
    git(r, 'mv', 'src/m.h', 'src/n.h')
    os.remove(os.path.join(r, 'docs', 'gone.md'))
    rc, out = cli(r, base)
    ok(rc == 0, 'rename + deletion: a `git mv` and an intended deletion exit 0 (exit %d)%s' % (rc, '' if rc == 0 else ': ' + ' | '.join(ln for ln in out.splitlines() if 'FAIL' in ln or 'BLOCKED' in ln)[:200]))


def main():
    global TMP
    only = sys.argv[1] if len(sys.argv) > 1 else None
    TMP = tempfile.mkdtemp(prefix='hbk_selftest_')
    import check_cdfields   # the scratch repos are not the tree the extractors read: in-process, the cd_ rows see no rows unless t_cdfields plants some
    for _n in list(check_cdfields.COLLECT):
        check_cdfields.COLLECT[_n] = lambda ref: []
    try:
        for name, fn in (('control', t_control), ('game_text', t_game_text), ('new_src', t_new_src), ('log_readers', t_log_readers), ('lint_runner', t_lint_runner),
                         ('runs_all', t_runs_all), ('gates', t_gates), ('cdfields', t_cdfields), ('list', t_list_complete), ('verdicts', t_verdicts), ('clean', t_clean), ('rename', t_rename_delete)):
            if only and only != name:
                continue
            try:
                fn()
            except Exception as e:   # a case that crashes is a FAIL, never a skip
                import traceback
                traceback.print_exc()
                ok(False, '%s: crashed: %s' % (name, e))
    finally:
        shutil.rmtree(TMP, ignore_errors=True)
    print('PASS: every case' if not FAILS else 'FAIL: %d case(s)' % len(FAILS))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
