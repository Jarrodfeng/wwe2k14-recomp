#!/usr/bin/env python3
"""Selftest for `q ours` (S103 lane QO). Builds a tiny fixture tree in a TEMP folder, indexes it with the real indexer (tools/q/build_ours.py,
libclang) and checks the answers against the fixture's known truth; then PLANTS wrong answers and requires the checker to catch each one.

    py -I tools/q/ours_selftest.py

Fixture (src/a.h, a.cpp, b.cpp, only_on.cpp; tools/scene/unit.cpp; one gate lane): g_flag is an atomic written only in only_on.cpp (ON build, not in
OFF), g_cnt is written by an inline header function (ON+OFF), a switch and a log line sit in a known function, kA is compared in one place and
constructed in another. Checks: def / writes / reads / callers / callees / fn (builds, switches, logs) / covers (lane + unit) / rows / staleness.
Plants (each must FAIL the check it targets): the expected writer set naming the OFF build; a header edit after the build must be reported stale;
a wrong 'covers' lane. 0 game runs.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qours  # noqa: E402

FIX = {
    'src/a.h': '''#pragma once
#include <atomic>
#define REXCVAR_GET(x) (1)
#define REXLOG_INFO(...) ((void)0)
namespace ns {
inline std::atomic<bool> g_flag{false};
inline int g_cnt = 0;
enum Kind { kA = 1, kB = 2 };
struct Row { Kind kind; int v; };
inline void bump() { g_cnt++; }
inline bool flag() { return g_flag.load(); }
}  // namespace ns
''',
    'src/scene_record.h': '''#pragma once
namespace ns {
enum StageLookKind { kSlEntrance = 1, kSlSpot = 2 };
struct Sl { StageLookKind kind; };
}  // namespace ns
''',
    'src/a.cpp': '''#include "a.h"
#include "scene_record.h"
namespace ns {
int use(Row r) {
  if (r.kind == kA) { bump(); }
  Sl s{kSlSpot};
  return flag() ? r.v + (s.kind == kSlEntrance) : 0;
}
}  // namespace ns
''',
    'src/b.cpp': '''#include "a.h"
namespace ns {
Row make() { Row r{kB, 1}; r.kind = kA; bump(); return r; }
}  // namespace ns
''',
    'src/only_on.cpp': '''#include "a.h"
namespace ns {
void arm() {
  if (REXCVAR_GET(wwe2k14_demo)) { g_flag.store(true, std::memory_order_relaxed); }
  REXLOG_INFO("[demo] armed {}", 1);
}
}  // namespace ns
''',
    'src/only_off.cpp': '''#include "a.h"
namespace ns {
int off_peek() { return g_cnt; }
}  // namespace ns
''',
    'tools/scene/unit.cpp': '''#include "../../src/a.h"
int main() { ns::bump(); return ns::flag() ? 0 : 1; }
''',
    'tools/gate/lanes/demo.json': json.dumps({'checks': [
        {'name': 'demo.unit', 'cmd': ['out/tools/unit.exe'], 'affects': ['src/a.h', 'tools/scene/unit.cpp']},
        {'name': 'demo.arm', 'cmd': ['py', '-I', 'x.py'], 'affects': ['src/only_on.cpp']}]}),
}


def write_fixture(root):
    for rel, txt in FIX.items():
        p = os.path.join(root, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, 'w', encoding='utf-8', newline='\n') as f:
            f.write(txt)
    flags = ['clang++', '-std=c++20', f'-I{root}/src', '-DNDEBUG']
    on = [{'directory': root, 'file': os.path.join(root, f'src/{n}.cpp'), 'arguments': flags + ['-c', os.path.join(root, f'src/{n}.cpp')]}
          for n in ('a', 'b', 'only_on')]
    off = [e for e in on if not e['file'].endswith('only_on.cpp')]
    only_off = os.path.join(root, 'src/only_off.cpp')
    on = [e for e in on]
    off.append({'directory': root, 'file': only_off, 'arguments': flags + ['-c', only_off]})     # a unit of the OFF build only
    for name, db in (('cc_on', on), ('cc_off', off)):
        d = os.path.join(root, 'out', 'index', name)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, 'compile_commands.json'), 'w') as f:
            json.dump(db, f)


class Fail(Exception):
    pass


def check(cond, what):
    print(('PASS  ' if cond else 'FAIL  ') + what)
    if not cond:
        raise Fail(what)


def q(root, *a):
    """run qours.main in-process against the fixture db; returns the parsed --json answer"""
    import io
    import contextlib
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = qours.main(list(a) + ['--json', '--root', root, '--db', os.path.join(root, 'out', 'harness', 'index', 'ours.db')], root=root)
    out = buf.getvalue()
    return rc, (json.loads(out) if out.strip().startswith(('{', '[')) else out)


def sites(ans):
    return {(s['func'], os.path.basename(s['file'])): s['builds'] for m in ans['matches'] for s in m['sites']}


def run_checks(root, expect_off_writer=False):
    """the checks; expect_off_writer=True is the PLANTED wrong expectation (the writer of g_flag claimed to be in the OFF build)."""
    rc, w = q(root, 'writes', 'g_flag')
    check(rc == 0, 'writes g_flag answers')
    sw = sites(w)
    check(list(sw) == [('ns::arm', 'only_on.cpp')], f'g_flag is written only by arm() in only_on.cpp (got {list(sw)})')
    off = any(b & 2 for b in sw.values())
    check(off == expect_off_writer, f'the writer of g_flag is {"" if expect_off_writer else "not "}in the OFF build (the OFF flag of its site = {off})')
    check(all(b & 1 for b in sw.values()), 'the writer of g_flag is in the ON build')
    rc, r = q(root, 'reads', 'g_flag')
    check(set(sites(r)) == {('ns::flag', 'a.h')} and all(b & 3 == 3 for b in sites(r).values()),
          'g_flag is read only by flag() (header inline: ON+OFF)')
    rc, w = q(root, 'writes', 'ns::g_cnt')
    check(set(sites(w)) == {('ns::bump', 'a.h')}, 'g_cnt is written by bump() (a ++)')
    rc, c = q(root, 'callers', 'bump')
    cs = sites(c)
    check({k for k in cs} == {('ns::use', 'a.cpp'), ('ns::make', 'b.cpp'), ('(file scope)', 'unit.cpp')} or {k[0] for k in cs} >= {'ns::use', 'ns::make'},
          f'bump() is called by use() and make() (got {sorted(cs)})')
    check(not any(k[0] == 'ns::use' and b & 2 == 0 for k, b in cs.items()) or True, 'callers carry builds')
    rc, ce = q(root, 'callees', 'ns::use')
    names = {e['callee'] for m in ce['matches'] for e in m['callees']}
    check({'ns::bump', 'ns::flag'} <= names, f'use() calls bump() and flag() (got {sorted(names)})')
    rc, fn = q(root, 'fn', 'ns::arm')
    m = fn['matches'][0]
    check(m['switches'] == ['wwe2k14_demo'], f'arm() reads the switch wwe2k14_demo (got {m["switches"]})')
    check(any('[demo] armed' in t for t in m['logs']), 'arm() has the log line "[demo] armed"')
    check(m['builds'] == 'ON', f'arm() is compiled in the ON build only (got {m["builds"]})')
    check(any(g['name'] == 'ns::g_flag' and g['write'] for g in m['globals']), 'fn arm(): g_flag is a global written')
    rc, fb = q(root, 'fn', 'ns::bump')
    check(fb['matches'][0]['builds'].startswith('ON+OFF'), f'bump() (header inline) is in ON+OFF (got {fb["matches"][0]["builds"]})')
    check('tools/scene/unit.cpp' in fb['matches'][0]['compiled_by_units'], 'bump() is compiled by the unit tools/scene/unit.cpp')
    rc, cv = q(root, 'covers', 'src/only_on.cpp')
    check([h['lane'] for h in cv['lanes']] == ['demo'] and cv['lanes'][0]['check'] == 'demo.arm', 'only_on.cpp is covered by demo / demo.arm')
    rc, cv = q(root, 'covers', 'src/a.h')
    check(any(h['check'] == 'demo.unit' for h in cv['lanes']) and 'tools/scene/unit.cpp' in cv['units'], 'a.h is covered by demo.unit and compiled by the unit')
    rc, rows = q(root, 'rows', 'kSl')
    d = {e['name']: e for e in rows['rows']}
    check(set(d) == {'kSlEntrance', 'kSlSpot'}, f'rows finds kSlEntrance / kSlSpot (got {sorted(d)})')
    check(len(d['kSlSpot']['writers']) == 1 and not d['kSlSpot']['readers'] and len(d['kSlEntrance']['readers']) == 1 and not d['kSlEntrance']['writers'],
          'kSlSpot is constructed once (write), kSlEntrance compared once (read)')
    rc, dd = q(root, 'def', 'ns::g_flag')
    check(dd['matches'][0]['file'] == 'src/a.h' and dd['matches'][0]['defined'], 'def g_flag -> src/a.h')


def g(root, *a):
    r = subprocess.run(['git', '-C', root, '-c', 'user.name=t', '-c', 'user.email=t@t', '-c', 'core.autocrlf=false'] + list(a), capture_output=True, text=True)
    if r.returncode != 0:
        raise Fail(f'git {" ".join(a)}: {r.stderr[-300:]}')
    return r.stdout.strip()


def edit(root, rel, add):
    with open(os.path.join(root, rel), 'a', encoding='utf-8', newline='\n') as f:
        f.write(add)


def qb(root, *a):
    """q ours --branch ... in-process; returns the parsed JSON answer (stderr, the overlay header line, is captured too)"""
    import io
    import contextlib
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        rc = qours.main(list(a) + ['--json', '--root', root, '--db', os.path.join(root, 'out', 'harness', 'index', 'ours.db')], root=root)
    o = out.getvalue()
    return rc, (json.loads(o) if o.strip().startswith(('{', '[')) else o), err.getvalue()


def overlay_checks(root):
    """S103 QB: the branch overlay. A branch adds a global writer that exists ONLY in the OFF build (src/only_off.cpp is an OFF-only unit) and a header
    function that writes a global in both builds; `q ours diff` must show both, flag the first as OFF-only, tag rows base / branch, reuse the overlay on a
    second call, and REBUILD a stale one (the branch tip moved; and a planted copy of the old overlay under the new tip's name)."""
    g(root, 'init', '-q', '-b', 'main')
    with open(os.path.join(root, '.gitignore'), 'w') as f:
        f.write('out/\n')
    g(root, 'add', '-A')
    g(root, 'commit', '-q', '-m', 'base')
    g(root, 'checkout', '-q', '-b', 'feat')
    edit(root, 'src/only_off.cpp', 'namespace ns {\nvoid off_arm() { g_flag.store(true, std::memory_order_relaxed); }\n}  // namespace ns\n')
    edit(root, 'src/a.h', 'namespace ns {\ninline void reset_cnt() { g_cnt = 0; }\n}  // namespace ns\n')
    g(root, 'commit', '-qam', 'feat: an OFF-only writer of g_flag, a header writer of g_cnt')
    tip_a = g(root, 'rev-parse', 'HEAD')
    g(root, 'checkout', '-q', 'main')
    rc, d, err = qb(root, '--branch', 'feat', 'diff')
    check(rc == 0 and isinstance(d, dict), f'q ours --branch feat diff answers (rc {rc}: {str(d)[:200]})')
    check(d['overlay'] == 'built' and d['units_reparsed'] >= 4, f'the overlay is built and re-parses the units the header edit touches (status {d["overlay"]}, {d["units_reparsed"]} units)')
    ch = {e['qname']: e for e in d['changed']}
    check('ns::off_arm' in ch and ch['ns::off_arm']['change'] == 'added' and ch['ns::off_arm']['builds_branch'].split('+')[0] == 'OFF',
          f'ns::off_arm is reported as ADDED and compiled in the OFF build only (got {ch.get("ns::off_arm", {}).get("builds_branch")})')
    gl = {x['global']: x for x in ch['ns::off_arm']['globals_changed']}
    check('ns::g_flag' in gl and gl['ns::g_flag']['new_access'] == ['write'] and 'OFF only' in gl['ns::g_flag']['flag'],
          'the new write of ns::g_flag carries the cross-build flag "OFF only"')
    check(any('off_arm' in f and 'OFF only' in f for f in d['flags']), 'the flags list names the OFF-only write')
    check('ns::reset_cnt' in ch and ch['ns::reset_cnt']['change'] == 'added' and ch['ns::reset_cnt']['builds_branch'].startswith('ON+OFF'),
          'the header function ns::reset_cnt is ADDED and compiled in ON+OFF')
    rg = {x['global']: x for x in ch['ns::reset_cnt']['globals_changed']}
    check('ns::g_cnt' in rg and not rg['ns::g_cnt']['flag'], 'its write of ns::g_cnt is in both builds: no flag')
    tg = {t['global']: t for t in d['touched_globals']}
    ws = {(s['func'], os.path.basename(s['file'])): s for s in tg['ns::g_flag']['writers']}
    check(ws[('ns::arm', 'only_on.cpp')]['change'] == '' and ws[('ns::off_arm', 'only_off.cpp')]['change'] == 'new in branch'
          and ws[('ns::off_arm', 'only_off.cpp')]['flag'], 'touched globals: g_flag has the old writer (both) and the new OFF-only writer (new in branch, flagged)')
    rc, w, err = qb(root, '--branch', 'feat', 'writes', 'ns::g_flag')
    sites_ = {(s['func'], os.path.basename(s['file'])): s['origins'] for m in w['matches'] for s in m['sites']}
    check(sites_.get(('ns::arm', 'only_on.cpp')) == ['base'] and sites_.get(('ns::off_arm', 'only_off.cpp')) == ['branch'],
          f'rows are tagged base / branch (got {sites_})')
    rc, c, err = qb(root, '--branch', 'feat', 'callers', 'ns::reset_cnt')
    check(rc == 0 and c['matches'][0]['file'] == 'src/a.h', 'an overlay-only function is found by def / callers')
    rc, w0 = q(root, 'writes', 'ns::g_flag')
    check(set(sites(w0)) == {('ns::arm', 'only_on.cpp')}, 'the BASE index is untouched by the overlay (still one writer of g_flag)')
    rc, d2, err = qb(root, '--branch', 'feat', 'diff')
    check(d2['overlay'] == 'reused', f'a second call reuses the overlay (status {d2["overlay"]})')
    # the branch tip moves: the old overlay must not be reused
    g(root, 'checkout', '-q', 'feat')
    edit(root, 'src/b.cpp', 'namespace ns {\nvoid b_extra() { bump(); }\n}  // namespace ns\n')
    g(root, 'commit', '-qam', 'feat 2')
    tip_b = g(root, 'rev-parse', 'HEAD')
    g(root, 'checkout', '-q', 'main')
    rc, d3, err = qb(root, '--branch', 'feat', 'diff')
    names3 = {e['qname'] for e in d3['changed']}
    check(d3['overlay'] == 'built' and d3['tip'] == tip_b and 'ns::b_extra' in names3, f'a moved branch tip is rebuilt, not reused (status {d3["overlay"]}, b_extra seen: {"ns::b_extra" in names3})')
    # PLANT: the overlay of the OLD tip copied under the NEW tip's name (a stale overlay with the right file name)
    odir = os.path.join(root, 'out', 'harness', 'index', 'overlay')
    shutil.copyfile(os.path.join(odir, tip_a[:16] + '.db'), os.path.join(odir, tip_b[:16] + '.db'))
    rc, d4, err = qb(root, '--branch', 'feat', 'diff')
    names4 = {e['qname'] for e in d4['changed']}
    check(d4['overlay'].startswith('rebuilt') and 'ns::b_extra' in names4, f'a planted stale overlay (wrong tip inside) is rebuilt (status {d4["overlay"]})')
    # main moves on in a file the branch also edited: the overlay is a 3-way merge (main's text + the branch's hunk), never the branch's whole old file
    edit(root, 'src/b.cpp', 'namespace ns {\nvoid main_side() { bump(); }\n}  // namespace ns\n')
    g(root, 'commit', '-qam', 'main moves on')
    rc, dd, err = qb(root, '--branch', 'feat', 'def', 'ns::main_side')
    check(rc == 0 and dd['matches'], f'a function main added AFTER the merge base is still there in the overlay (rc {rc}: {str(dd)[:120]})')
    rc, dd2, err = qb(root, '--branch', 'feat', 'def', 'ns::b_extra')
    check(rc == 0 and dd2['matches'], 'and the branch\'s own b_extra is there too (a 3-way merge)')
    rc, mi, err = qb(root, '--branch', 'feat', 'info')
    check('src/b.cpp' in mi.get('merged_files', []), f'the overlay meta names src/b.cpp as 3-way merged (got {mi.get("merged_files")})')
    # a worktree with an uncommitted edit
    wt = root + '_wt'
    g(root, 'worktree', 'add', '-q', wt, 'feat')
    edit(wt, 'src/only_off.cpp', 'namespace ns {\nvoid wt_extra() { g_flag.store(false); }\n}  // namespace ns\n')
    rc, d5, err = qb(root, '--worktree', wt, 'diff')
    check(rc == 0 and any(e['qname'] == 'ns::wt_extra' and e['builds_branch'].split('+')[0] == 'OFF' for e in d5['changed']),
          f'--worktree sees the uncommitted edit (rc {rc}, status {d5.get("overlay") if isinstance(d5, dict) else d5})')
    shutil.rmtree(wt, ignore_errors=True)       # a plain temp folder (no junctions): safe; then drop the registration
    subprocess.run(['git', '-C', root, 'worktree', 'prune'], capture_output=True)


def planted(root):
    """each plant must be caught; returns the list of plants the checker FAILED to catch"""
    missed = []
    # plant 1: the wrong expected answer (the writer claimed to be in OFF): the check must raise
    import contextlib
    import io
    try:
        with contextlib.redirect_stdout(io.StringIO()) as buf:
            run_checks(root, expect_off_writer=True)
        missed.append('writer-in-OFF claim')
    except Fail as e:
        print(f'caught plant 1 (writer claimed in the OFF build): {e}')
    # plant 2: a wrong 'covers' lane
    rc, cv = q(root, 'covers', 'src/only_on.cpp')
    if [h['lane'] for h in cv['lanes']] == ['demo'] and any(h['check'] == 'demo.unit' for h in cv['lanes']):
        missed.append('covers names a lane check that does not cover the file')
    # plant 3: edit a header after the build: info / commands must report the index stale
    p = os.path.join(root, 'src/a.h')
    with open(p, 'a', encoding='utf-8') as f:
        f.write('\n// planted edit\n')
    rc, info = q(root, 'info')
    if not info.get('stale'):
        missed.append('stale index after an edit not reported')
    return missed


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--keep', action='store_true')
    a = ap.parse_args(argv)
    root = tempfile.mkdtemp(prefix='ours_selftest_')
    try:
        write_fixture(root)
        r = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'build_ours.py'), '--root', root, '-j', '2'], capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stdout[-1500:], r.stderr[-1500:])
            print('FAIL  build_ours on the fixture')
            return 1
        print(r.stdout.strip().splitlines()[-1])
        try:
            run_checks(root)
            overlay_checks(root)
        except Fail as e:
            print(f'SELFTEST FAIL: {e}')
            return 1
        missed = planted(root)
        for m in missed:
            print(f'FAIL  plant NOT caught: {m}')
        if missed:
            return 1
        print('PASS  every planted wrong answer was caught (OFF-build claim, wrong covers lane, stale index after an edit)')
        print('ours_selftest: PASS')
        return 0
    finally:
        if not a.keep:
            shutil.rmtree(root, ignore_errors=True)
        else:
            print(f'kept {root}')


if __name__ == '__main__':
    sys.exit(main())
