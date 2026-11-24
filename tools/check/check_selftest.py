"""S106 C2 selftest (0 game runs): the check runner proves itself on S105's past faults and planted twins in the UNSAFE direction.

  py -I tools\\check\\check_selftest.py

Reads the main checkout's out\\bench mirrors and the saved S105 logs read-only; writes only under a temp folder. Past faults: run 185's recipe
(Cena's belt has no plate) is refused at preflight; log 110 reads as blood where S105 first read "no blood" through a cut. Twins: a missing line is
UNREAD (never a pass), a BEFORE with the fixed values is BEFORE-BLIND (never PASS), a wrong exe / a missing mirror / captions off / alone are refused.
"""
# entry: main
import argparse
import copy
import io
import contextlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import check as C  # noqa: E402
import readout as RO  # noqa: E402
import recipe as R  # noqa: E402

MAIN = C.find_main()
LOGS = os.path.join(MAIN, 'out', 'build', 'win-amd64-release-off', 'logs')
FAILS = []


def ok(cond, what):
    print('%s  %s' % ('ok  ' if cond else 'FAIL', what))
    if not cond:
        FAILS.append(what)


def ns(**kw):
    d = dict(main=None, session='s106', bench=None, runs_log=None)
    d.update(kw)
    return argparse.Namespace(**d)


def rec_of(row, edit=None):
    rec = R.load(os.path.join(HERE, 'recipes', row + '.toml'))
    if edit:
        edit(rec)
    return rec


def lines_of(n):
    p = os.path.join(LOGS, 'wwe2k14_%d.log' % n)
    return RO.read_log(p) if os.path.isfile(p) else None


def verdict(row, arm, lines, answers=None):
    rec = rec_of(row)
    return RO.arm_verdict(rec, arm, RO.evaluate(rec, lines), answers)


def fake_exe(root, preset, commit, dirty=0, info=None):
    d = os.path.join(root, preset)
    os.makedirs(os.path.join(d, 'stamp'), exist_ok=True)
    with open(os.path.join(d, 'stamp', 'wwe2k14_build_stamp.h'), 'w') as f:
        f.write('#define WWE2K14_BUILD_GIT "%s"\n#define WWE2K14_BUILD_DIRTY %d\n' % (commit, dirty))
    exe = os.path.join(d, 'wwe2k14.exe')
    with open(exe, 'wb') as f:
        f.write(b'MZ' + commit.encode() + b'\0' + b'x' * 64)
    if info is not None:
        with open(os.path.join(d, 'exe_info.json'), 'w') as f:
            json.dump(info, f)
    return exe


def main():
    if not os.path.isdir(os.path.join(MAIN, 'out', 'bench', 'userdata_frozen')):
        print('BLOCKED: no %s (the frozen mirror)' % os.path.join(MAIN, 'out', 'bench', 'userdata_frozen'))
        return 2
    os.makedirs(os.path.join(C.TREE, 'out'), exist_ok=True)
    tmp = tempfile.mkdtemp(prefix='chk_selftest_', dir=os.path.join(C.TREE, 'out'))   # under out\: make_test_save refuses a DST elsewhere
    try:
        return run(tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def run(tmp):
    runs_log = os.path.join(tmp, 'check_runs.log')
    ctx = C.Ctx(ns(runs_log=runs_log, bench=os.path.join(tmp, 'bench')))
    os.makedirs(ctx.bench)
    real_ctx = C.Ctx(ns(runs_log=runs_log))      # reads the main checkout's frozen mirror
    real_ctx.sdir = os.path.join(tmp, 'sdir_real')      # S106 C2F F8: never the real out\s106\check\<row>\mirror.sha256
    ctx.sdir = os.path.join(tmp, 'sdir_ctx')

    # ---- bars
    ok(R.meets(3, '> 0') and not R.meets(0, '> 0') and R.meets(0, '== 0') and R.meets(0x7C, '& 0x40') and not R.meets(0x3C, '& 0x40'), 'bars: > == &')
    ok(R.meets(frozenset({3, 4}), '== {3,4}') and not R.meets(frozenset({2, 3, 4}), '== {3,4}') and not R.meets(None, '>= 0'), 'bars: set, UNREAD never meets')

    # ---- past fault 1: run 185's recipe (Cena's belt) refused at preflight, 0 runs
    c185 = rec_of('PLATE', lambda r: r['destination'].update(champion={'title': 0, 'char': 139, 'plate': True}))
    code, msgs, _ = C.preflight(real_ctx, c185, 'before')
    ok(code == 3 and any('FALSE' in m and 'plate:title=0' in m for m in msgs), 'run 185 recipe (Cena, title 0): REFUSED, plate:title=0 FALSE (exit %d)' % code)
    code, msgs, _ = C.preflight(real_ctx, rec_of('PLATE', lambda r: r['destination'].update(champion={'title': 1, 'char': 139})), 'before')
    ok(code == 3 and any('holder:title=1:char=139' in m and 'FALSE' in m for m in msgs), 'wrong holder of title 1 (139): REFUSED (exit %d)' % code)
    code, msgs, _ = C.preflight(real_ctx, rec_of('PLATE'), 'before')
    ok(code == 0, 'the right recipe (322, title 1) passes preflight (exit %d)' % code)

    # Cena's HIAC launch (a route with no match line, the scenario names the participants): 322 is not among them
    rec = rec_of('PLATE', lambda r: r['destination'].update(route='tools\\check\\recipes\\A8.toml', flags='--wwe2k14_launch=hiac_cena_brodus'))
    ids, src = R.participants(rec, MAIN, C.TREE)
    if os.path.isfile(os.path.join(MAIN, 'out', 'scenarios', 'hiac_cena_brodus.toml')):
        ok(ids is not None and 139 in ids and 322 not in ids, 'participants from the scenario toml: %s' % sorted(ids or []))
    ok(R.participants(rec_of('PLATE'), MAIN, C.TREE)[0] == {322, 324}, 'participants from the route cvar match line')
    nop = rec_of('PLATE', lambda r: r['destination'].update(route=r['destination']['route']))
    nop['destination']['route'] = 'tools\\check\\recipes\\A8.toml'
    ok(R.participants(nop, MAIN, C.TREE)[0] is None, 'no participants visible -> None (champion recipe refused)')

    # ---- captions off / planted
    off = rec_of('A8', lambda r: (r.pop('plant'), r['destination'].update(data='userdata_frozen')))
    code, msgs, _ = C.preflight(real_ctx, off, 'before')
    ok(code == 3 and any('option:caption=1' in m and 'FALSE' in m for m in msgs) and any('plant it' in m for m in msgs),
       'captions off, no plant: REFUSED with the hint (exit %d)' % code)
    code, msgs, _ = C.preflight(ctx, rec_of('A8'), 'before')
    ok(code == 0, 'captions planted by the recipe: preflight passes on the TEST mirror (exit %d: %s)' % (code, ' | '.join(msgs)[:200]))
    ok(os.path.isdir(os.path.join(ctx.bench, 'userdata_chk_A8')), 'the plant went under the bench folder, not the frozen mirror')
    # planted holder (C1F --holder): a champion recipe on a mirror whose holder is wrong is fixed by the plant
    hp = rec_of('PLATE', lambda r: (r['destination'].update(data='plant', champion={'title': 1, 'char': 324}), r.update(plant={'threads': 'none', 'holder': '1:324'})))
    code, msgs, _ = C.preflight(ctx, hp, 'before')
    ok(any('true' in m and 'holder:title=1:char=324' in m for m in msgs), 'holder plant (C1F --holder 1:324): the planted holder reads true on the TEST mirror (exit %d)' % code)
    bad_h = rec_of('PLATE', lambda r: r['destination'].update(champion={'title': 1, 'char': 324}))
    ok(C.preflight(real_ctx, bad_h, 'before')[0] == 3, 'the same champion without the plant: REFUSED (frozen holder is 322)')

    # ---- mirror missing is exit 2; alone refused
    code, msgs, _ = C.preflight(ctx, rec_of('A7', lambda r: r['destination'].update(data='userdata_nonexistent')), 'before')
    ok(code == 2 and 'no user-data mirror' in ' '.join(msgs), 'missing mirror: exit 2, refused (%s)' % msgs[:1])
    ctx_noalone = C.Ctx(ns(runs_log=runs_log, bench=ctx.bench, main=tmp))      # a main with no run_list: no -Alone
    code, msgs, _ = C.preflight(ctx_noalone, rec_of('PLATE', lambda r: r.setdefault('run', {}).update(alone=True)), 'before')
    ok(code == 3 and 'alone not supported yet' in ' '.join(msgs), 'alone = true without run_list -Alone: REFUSED, never silently shared')
    # a mods / file precondition
    code, msgs, _ = C.preflight(real_ctx, rec_of('A7', lambda r: r['pre'].append({'file': 'out\\nope\\nothing'})), 'before')
    ok(code == 3 and 'does not exist' in ' '.join(msgs), 'a missing required file: REFUSED')

    # ---- exe checks
    exes = os.path.join(tmp, 'exes')
    good = fake_exe(exes, 'win-amd64-release-off', 'abcdef123456')
    rec = rec_of('A12')
    ok(C.exe_refusal(rec, good, 'abcdef1')[0] is None, 'exe: stamp commit == the arm commit passes')
    ok('not the arm' in (C.exe_refusal(rec, good, '999999')[0] or ''), 'exe: a wrong commit is refused')
    ok('dirty' in (C.exe_refusal(rec, fake_exe(os.path.join(tmp, 'e2'), 'win-amd64-release-off', 'abcdef123456', 1), 'abcdef1')[0] or ''), 'exe: built dirty is refused')
    ok('preset' in (C.exe_refusal(rec, fake_exe(os.path.join(tmp, 'e3'), 'win-amd64-release', 'abcdef123456'), 'abcdef1')[0] or ''), 'exe: the wrong preset is refused')
    a = fake_exe(os.path.join(tmp, 'e4'), 'win-amd64-release-off', 'abcdef123456', info={'commit': 'abcdef123456', 'preset': 'win-amd64-release-off', 'options': {'WWE2K14_X': 'ON'}})
    b = fake_exe(os.path.join(tmp, 'e5'), 'win-amd64-release-off', 'abcdef123456', info={'commit': 'abcdef123456', 'preset': 'win-amd64-release-off', 'options': {'WWE2K14_X': 'OFF'}})
    ia = C.exe_info(a)[0]
    ok(C.exe_refusal(rec, a, 'abcdef1', ia)[0] is None, 'exe_info.json: equal options pass')
    ok('options differ' in (C.exe_refusal(rec, b, 'abcdef1', ia)[0] or ''), 'exe_info.json: the other arm built with different options is refused')
    ok('does not exist' in (C.exe_refusal(rec, os.path.join(tmp, 'none.exe'), 'abc')[0] or ''), 'exe: missing file refused')
    ok('no --commit' in (C.exe_refusal(rec, good, None)[0] or ''), 'exe: no commit named is refused')

    # ---- ForCommit: a real run needs run_list -ForCommit; a dry run does not
    fm = os.path.join(tmp, 'fm')
    os.makedirs(os.path.join(fm, 'tools', 'bench'))
    open(os.path.join(fm, 'tools', 'bench', 'run_list.ps1'), 'w').write('param([string[]]$Scripts, [switch]$AllowStaleExe)\n')
    fctx = C.Ctx(ns(runs_log=runs_log, bench=real_ctx.bench, main=fm))
    code, msgs, _ = C.preflight(fctx, rec_of('PLATE'), 'before', good, 'abcdef1', real=True)
    ok(code == 3 and 'ForCommit' in ' '.join(msgs), 'real run: run_list without -ForCommit is refused (the exe stamp would go unchecked)')
    open(os.path.join(fm, 'tools', 'bench', 'run_list.ps1'), 'w').write('param([string[]]$Scripts, [string]$ForCommit = "")\n')
    code, msgs, _ = C.preflight(fctx, rec_of('PLATE'), 'before', good, 'abcdef1', real=True)
    ok(code == 0, 'real preflight with -ForCommit present passes (exit %d: %s)' % (code, msgs[:2]))

    # ---- readouts on the saved S105 logs
    L = {n: lines_of(n) for n in (109, 110, 111, 133, 182, 184, 185, 186)}
    if any(v is None for v in L.values()):
        print('BLOCKED: saved S105 logs missing in %s' % LOGS)
        return 2
    ev = RO.evaluate(rec_of('A12'), L[110])
    vals = {r['name']: r['value'] for r in ev['readouts']}
    ok(vals == {'host items skipped for no picture': 0, 'hook picture reads failed': 0, 'handed to the skin': 2} and all(g['ok'] for g in ev['guards']),
       'A12 on log 110 reads blood: no_picture 0, tex_fail 0, handed 2, guard ok (%s)' % vals)
    ok(verdict('A12', 'after', L[110]) == 'PASS', 'A12 AFTER on log 110 = PASS')
    ok(verdict('A12', 'before', L[110]) == 'BEFORE-BLIND', 'A12 BEFORE on log 110 (the fixed values) = BEFORE-BLIND, never PASS')
    # planted twins
    bad = [l.replace('no_picture 0', 'no_picture 7').replace('tex_fail 0', 'tex_fail 9') if 'tex_fail' in l or 'no_picture' in l else l for l in L[110]]
    ok(verdict('A12', 'before', bad) == 'FAULT' and verdict('A12', 'after', bad) == 'FAIL', 'twin: fault values -> BEFORE FAULT, AFTER FAIL')
    nofix = [l for l in L[110] if 'tex_fail' not in l]
    ok(verdict('A12', 'after', nofix) == 'VOID:readout' and verdict('A12', 'before', nofix) == 'VOID:readout', 'twin: a readout line that never appears = VOID:readout, never a pass')
    nomark = [l for l in L[110] if 'bleed slot 0 level 3: ok' not in l]
    ok(verdict('A12', 'after', nomark) == 'VOID:guard', 'twin: the verb never ran = VOID:guard')
    ok(verdict('A12', 'after', []) == 'VOID:guard', 'twin: an empty log = VOID:guard')
    ok(RO.aggregate(L[110], {'line': 'zzz never', 'agg': 'count'}) == 0 and RO.aggregate(L[110], {'line': r'zzz (?P<v>\d+)', 'agg': 'max'}) is None, 'count 0 vs UNREAD')
    # A5d / A8 / PLATE / IMP / A7
    ok(verdict('A5d', 'after', L[109]) == 'PASS', 'A5d AFTER on log 109 (magenta key) = PASS')
    ok(verdict('A5d', 'before', L[109]) == 'BEFORE-BLIND', 'A5d BEFORE on log 109 = BEFORE-BLIND')
    ok(verdict('A8', 'after', L[133], 'yes') == 'PASS' and verdict('A8', 'before', L[133], 'yes') == 'BEFORE-BLIND',
       'A8 on log 133: holes 0 + caption seen (eyes yes) -> AFTER PASS, BEFORE blind (S106 C2F B2: the count alone decides nothing)')
    v185 = verdict('PLATE', 'after', L[185])
    ok(v185 == 'VOID:guard', 'run 185 (Cena match) through the PLATE recipe: VOID:guard (champion 322 not in the card), not a pass (%s)' % v185)
    ok(verdict('PLATE', 'after', L[186]) == 'EYES' and verdict('PLATE', 'after', L[186], 'yes') == 'PASS', 'PLATE on log 186: mechanism met, shots decide: EYES then yes = PASS')
    ok(verdict('PLATE', 'after', L[186], 'no') == 'FAIL' and verdict('PLATE', 'before', L[186]) == 'EYES' and verdict('PLATE', 'before', L[186], 'no') == 'FAULT'
       and verdict('PLATE', 'before', L[186], 'yes') == 'BEFORE-BLIND', 'PLATE: shots-only fault side: no = FAULT, yes = BEFORE-BLIND')
    ok(verdict('IMP', 'after', L[184], 'yes') == 'PASS', 'IMP AFTER on log 184 (fix) with eyes yes = PASS (guard set {2..6} read)')
    ok(verdict('IMP', 'after', L[182], 'yes') == 'FAIL', 'IMP AFTER on log 182 (no fix): no re-offered line = FAIL')
    ok(verdict('A7', 'after', L[111]) == 'PASS', 'A7 AFTER on log 111 (loaded from mod) = PASS')

    # ---- compare
    cmp_ = RO.compare
    ok(cmp_('FAULT', 'PASS') == 'PASS' and cmp_('FAULT', 'FAIL') == 'FAIL' and cmp_('FAULT', 'FLAG') == 'FLAG', 'compare: FAULT + PASS/FAIL/FLAG')
    ok(cmp_('BEFORE-BLIND', 'PASS') == 'BEFORE-BLIND', 'compare: BEFORE without the fault + AFTER fixed = BEFORE-BLIND, never PASS')
    ok(cmp_('EYES', 'PASS') == 'EYES' and cmp_('VOID:guard', 'PASS') == 'VOID' and cmp_('FAULT', None) == 'PENDING' and cmp_(None, 'PASS') == 'PENDING', 'compare: EYES / VOID / PENDING')

    # ---- log line format + exact row match
    c2 = C.Ctx(ns(runs_log=os.path.join(tmp, 'x', 'l.log'), main=tmp))
    l1 = C.log_line(c2, 'A12', 'before', 'pending', 'pending', 'q\r\nwith run= and verdict= inside')
    l2 = C.log_line(c2, 'A1', 'after', 'e:001', 'PASS', 'one')
    ok(re.match(r'^\d{4}-\d\d-\d\d \d\d:\d\d:\d\d row=A12 arm=BEFORE run=pending verdict=pending question=q with run: and verdict: inside$', l1) is not None, 'log line format fixed')
    ok(len(C.runs_for(c2.runs_log, 'A1')) == 1 and len(C.runs_for(c2.runs_log, 'A12')) == 0 and C.runs_for(c2.runs_log, 'A1')[0] == l2,
       'row=A1 does not match row=A12; a pending line is not a check')

    # ---- eyes + verdict + sheet on a result folder
    rt = os.path.join(tmp, 'rt')
    rc = C.Ctx(ns(runs_log=os.path.join(tmp, 'rt.log'), main=rt))
    rec = rec_of('PLATE')
    for arm, lg in (('before', L[186]), ('after', L[186])):
        e = RO.evaluate(rec, lg)
        C.save_result(rc, 'PLATE', arm, dict(row='PLATE', arm=arm, run='x:186', verdict=RO.arm_verdict(rec, arm, e), commit='abc', ev=e, tripwire_new=0, void=None))
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        c1 = C.main(['eyes', 'PLATE', '--arm', 'before', '--answer', 'no', '--main', rt, '--runs-log', rc.runs_log])
        c2_ = C.main(['eyes', 'PLATE', '--arm', 'after', '--answer', 'yes', '--main', rt, '--runs-log', rc.runs_log])
        c3 = C.main(['verdict', 'PLATE', '--main', rt, '--runs-log', rc.runs_log])
    ok((c1, c2_, c3) == (0, 0, 0) and 'row PLATE: PASS' in out.getvalue(), 'eyes before no + after yes -> verdict PASS (exits %s)' % ((c1, c2_, c3),))
    ok(len(C.runs_for(rc.runs_log, 'PLATE')) == 2 and os.path.isfile(os.path.join(rc.row_dir('PLATE'), 'verdict.txt')) and os.path.isfile(os.path.join(rc.row_dir('PLATE'), 'sheet.jpg')),
       'eyes wrote one line per answer, verdict.txt and sheet.jpg exist')

    # ---- dry run: prints launch.ps1 for each arm, never writes the log
    dl = os.path.join(tmp, 'dry.log')
    p = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'check.py'), 'run', 'PLATE', '--dry', '--runs-log', dl, '--main', MAIN, '--session', 's106dry'],
                       capture_output=True, text=True, timeout=120)
    o = p.stdout
    ok(p.returncode == 0 and o.count('run_list.ps1') >= 2 and 'arm BEFORE' in o and 'arm AFTER' in o and '-ForCommit' in o and '-AllowStaleExe' not in o and not os.path.exists(dl)
       and not os.path.isdir(os.path.join(MAIN, 'out', 's106dry')), 'run --dry: launch.ps1 per arm printed (-ForCommit, no -AllowStaleExe), nothing written (exit %d)' % p.returncode)
    # a refusal never reaches the log
    ok(not os.path.exists(runs_log), 'no refusal wrote a line to check_runs.log')

    # ---- lint
    ok(R.lint(rec_of('A5d', lambda r: r['readout'][0].update(line='(')), C.TREE)[0] != [], 'lint: a regex that does not compile is an error')
    ok(any('fault-side' in e for e in R.lint(rec_of('PLATE', lambda r: r.pop('shots')), C.TREE)[0]), 'lint: no fault-side item is an error')
    ok(any('does not exist' in e for e in R.lint(rec_of('A12', lambda r: r['destination'].update(route='tools\\nope.txt')), C.TREE)[0]), 'lint: a missing route is an error, not a crash')
    ok(R.lint(rec_of('A12', lambda r: r['recipe'].update(row='A 1')), C.TREE)[0] != [], 'lint: a row with a space is an error')
    ok(R.lint(rec_of('A12', lambda r: r['destination'].update(flags='--user_data_root=x')), C.TREE)[0] != [], 'lint: --user_data_root in flags is an error')

    c2f_rows(tmp, runs_log, real_ctx, L)

    print('\n%s' % ('check_selftest: ALL PASS' if not FAILS else 'check_selftest: %d FAIL: %s' % (len(FAILS), '; '.join(FAILS))))
    return 1 if FAILS else 0


def T(what, fn):
    try:
        ok(bool(fn()), what)
    except Exception as e:  # noqa: BLE001
        ok(False, '%s (raised %s: %s)' % (what, type(e).__name__, e))


def cmd_quiet(fn, ctx, a):
    o = io.StringIO()
    with contextlib.redirect_stdout(o):
        code = fn(ctx, a)
    return code, o.getvalue()


def c2f_rows(tmp, runs_log, real_ctx, L):
    """S106 C2F: one row per finding of docs/research/s106/review_C2.md (B1-B3, F1-F14), each written first and seen to give the wrong verdict on the old code."""
    real_ctx.sdir = os.path.join(tmp, 'sdir_real')                         # F8: never read / write the real out\s106
    head = subprocess.run(['git', '-C', C.TREE, 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
    goodx = fake_exe(os.path.join(tmp, 'exes2'), 'win-amd64-release-off', head,
                     info={'commit': head, 'preset': 'win-amd64-release-off', 'options': {'WWE2K14_X': 'ON'}})

    def mk_main(name):
        d = os.path.join(tmp, name)                                        # a main whose run_list has -ForCommit (real runs refuse otherwise)
        os.makedirs(os.path.join(d, 'tools', 'bench'))
        with open(os.path.join(d, 'tools', 'bench', 'run_list.ps1'), 'w') as f:
            f.write('param([string[]]$Scripts, [string]$ForCommit = "")\n')
        return d
    envm = mk_main('envm')

    def cns(recipe, arm=None, exe=None, commit=None, dry=False, **kw):
        d = dict(recipe=recipe, arm=arm, exe=exe, commit=commit, dry=dry, main=envm, session='s106', bench=real_ctx.bench,
                 runs_log=os.path.join(tmp, 'envrun.log'), answer=None, no_sheet=True)
        d.update(kw)
        return argparse.Namespace(**d)

    def rcmd(recipe, **kw):
        a = cns(recipe, **kw)
        return cmd_quiet(C.cmd_run, C.Ctx(a), a)[0]

    L2 = {n: lines_of(n) for n in (104, 122)}
    l110, l133 = L[110], L[133]

    # ---- B1: a crashed or cut run can never read PASS
    T('B1 run_list line with exit code -1073741819 -> VOID:route', lambda: C.parse_run_list('x.txt -> wwe2k14_133  EXITED after 40.4s, exit code -1073741819 (0xC0000005)')[1] == 'route')
    T('B1 run_list line with exit code 0 is not void', lambda: C.parse_run_list('x.txt -> wwe2k14_133  EXITED after 40.4s, exit code 0 (0x00000000)') == ('wwe2k14_133', None))
    T('B1 run_list with no EXITED line (cut) -> VOID:route', lambda: C.parse_run_list('x.txt -> wwe2k14_133')[1] == 'route')
    # S106 C5F B1: a run_list with `alone:` lines that say the PC was not alone is BLOCKED-ALONE (readout not judged), never judged
    _ok = 'x.txt -> wwe2k14_133  EXITED after 40.4s, exit code 0 (0x00000000)\n'
    T('C5F B1 planted `alone: broken` line -> void alone, verdict BLOCKED-ALONE', lambda: C.parse_run_list(_ok + 'alone: broken at 19:50:40 (clang++.exe pid 1234, seen 19:50:40-19:50:52): readout not judged')[1] == 'alone'
      and RO.arm_verdict({}, 'after', dict(guards=[], readouts=[]), None, 0, 'alone') == 'BLOCKED-ALONE')
    T('C5F B1 planted `alone: NO` line -> void alone', lambda: C.parse_run_list('alone: NO after 300 s (games 0; compiles 6: x): not run\nABORT: alone')[1] == 'alone')
    # S106 C5FF N1: run_list.ps1 prints the refusal as `ABORT: alone: NO ...` (its catch block), never `alone: NO` at the start of a line
    T('C5FF N1 the REAL run_list refusal text (`ABORT: alone: NO ...`) -> void alone (BLOCKED-ALONE), not abort', lambda: C.parse_run_list("ABORT: alone: NO after 300 s (games 0; compiles 6: admit cc.tool.0 pid 1 for pid 2 'x', clang++.exe x3): not run")[1] == 'alone')
    T('C5FF N1 `ABORT: alone: NO after 0 s (could not take the pause file ...)` -> void alone', lambda: C.parse_run_list('ABORT: alone: NO after 0 s (could not take the pause file C:\\x\\alone.lock: another process holds it): not run')[1] == 'alone')
    T('C5FF N1 an ABORT that merely mentions alone stays abort', lambda: C.parse_run_list('ABORT: input script not found: alone: NOT a script')[1] == 'abort')
    T('C5F B1 planted `alone: unverified` line -> void alone', lambda: C.parse_run_list(_ok + 'alone: unverified (sampler: dead): readout not judged')[1] == 'alone')
    T('C5F B1 `alone: held` / `alone: yes` stay judged', lambda: C.parse_run_list(_ok + 'alone: yes (games 0, compiles 0, since 19:50:06)\nalone: held (games 0, compiles 0, 19:50:06-19:51:11, 33 samples)') == ('wwe2k14_133', None))
    bq = next(i for i, l in enumerate(l110) if 'bleed slot 0 level 3: ok' in l)
    liq = [i for i, l in enumerate(l110) if '[scene][maps][liquid]' in l and i > bq]
    T('B1 A12 AFTER on log 110 cut after the first liquid line (no quit line) = VOID:guard, not PASS', lambda: verdict('A12', 'after', l110[:liq[0] + 3]) == 'VOID:guard')
    cr = next(i for i, l in enumerate(l133) if 'scene=card_ready' in l)
    T('B1 A8 AFTER on log 133 cut 200 lines after scene=card_ready, eyes yes = VOID, not PASS', lambda: verdict('A8', 'after', l133[:cr + 200], 'yes').startswith('VOID'))
    T('B1 a script FAILED line in the log = VOID:guard', lambda: verdict('A12', 'after', l110 + ['[wwe2k14][input] script FAILED at line 3']) == 'VOID:guard')

    # ---- B2: A8's holes count is a mechanism; the fault side is shots only
    T('B2 A8 BEFORE on log 104 (captions off, holes 1; the run also failed: script FAILED, no quit line) is VOID:guard, never FAULT', lambda: verdict('A8', 'before', L2[104]) == 'VOID:guard')
    T('B2 A8 BEFORE on log 104 with its guards forced ok still is not FAULT (the count is a mechanism: EYES)', lambda: (lambda ev: RO.arm_verdict(
        rec_of('A8'), 'before', dict(ev, guards=[dict(g, ok=True) for g in ev['guards']])) == 'EYES')(RO.evaluate(rec_of('A8'), L2[104])))
    T('B2 A8 BEFORE on log 122 (captions off) is no longer FAULT by the count (EYES)', lambda: verdict('A8', 'before', L2[122]) == 'EYES')
    T('B2 A8 AFTER on log 133 without eyes = EYES (never PASS); eyes yes = PASS; eyes no = FAIL', lambda: (
        verdict('A8', 'after', l133) == 'EYES' and verdict('A8', 'after', l133, 'yes') == 'PASS' and verdict('A8', 'after', l133, 'no') == 'FAIL'))
    T('B2 A8 BEFORE eyes no = FAULT, eyes yes = BEFORE-BLIND', lambda: verdict('A8', 'before', l133, 'no') == 'FAULT' and verdict('A8', 'before', l133, 'yes') == 'BEFORE-BLIND')
    T('B2 A8 caption phase not reached (no chk_caption shots) = VOID:guard', lambda: verdict('A8', 'after', [l for l in l133 if 'chk_caption_' not in l], 'yes') == 'VOID:guard')
    T('B2 lint: a role = "fault" readout without specific = true / eyes = "required" is an error', lambda: any('specific' in e for e in R.lint(
        rec_of('A12', lambda r: [x.pop('specific', None) for x in r['readout']]), C.TREE)[0]))
    T('B2 recipe eyes = "required": a fault readout alone cannot PASS the AFTER', lambda: RO.arm_verdict(
        rec_of('A12', lambda r: r['recipe'].update(eyes='required')), 'after', RO.evaluate(rec_of('A12'), l110)) == 'EYES')

    # ---- B3: both arms the same except the exe
    base_rec = os.path.join(C.HERE, 'recipes', 'A12.toml')
    edited = os.path.join(tmp, 'A12_edited.toml')
    with open(base_rec, encoding='utf-8') as f, open(edited, 'w', encoding='utf-8') as g:
        g.write(f.read().replace('fixed = "== 0"', 'fixed = "<= 0"', 1))
    eres = mk_main('env_b3')

    def stored(recpath, arm, verdict_, main, commit=head, log=None):
        c = C.Ctx(cns(recpath, main=main))
        C.save_result(c, 'A12', arm, dict(row='A12', arm=arm, run='x:1', verdict=verdict_, commit=commit, ev=dict(guards=[], readouts=[], shots=[]), tripwire_new=0,
                                          void=None, log=log, exe_info=C.exe_info(goodx)[0], inputs=C.inputs_hash(c, R.load(recpath), arm)))

    def code_of(recpath, arm, main, commit=head):
        a = cns(recpath, arm=arm, exe=goodx, commit=commit, main=main)
        return cmd_quiet(C.cmd_run, C.Ctx(a), a)[0]

    def b3_refused():
        stored(edited, 'before', 'FAULT', eres)
        return code_of(base_rec, 'after', eres) == 3
    T('B3a an AFTER whose recipe differs from BEFORE\'s is REFUSED exit 3 (edited bar)', b3_refused)

    def ih(recpath, flags=None, arm='before'):
        rec_ = R.load(recpath)
        if flags:
            rec_['destination']['flags'] = flags
        return C.inputs_hash(C.Ctx(cns(recpath, main=eres)), rec_, arm)
    T('B3a inputs hash: equal across arms for one recipe; the recipe, the flags and the route are in it', lambda: ih(base_rec) == ih(base_rec, arm='after')
      and ih(base_rec)['recipe'] != ih(edited)['recipe'] and ih(base_rec)['flags'] != ih(base_rec, '--wwe2k14_fixed_seed=7')['flags'] and ih(base_rec).get('route'))

    def b3_verdict():
        stored(edited, 'before', 'FAULT', eres)
        stored(base_rec, 'after', 'PASS', eres)
        a = cns(base_rec, main=eres)
        return 'row A12: VOID' in cmd_quiet(C.cmd_verdict, C.Ctx(a), a)[1]
    T('B3a verdict VOIDs two arms of different input hashes', b3_verdict)
    stamp_only = fake_exe(os.path.join(tmp, 'e6'), 'win-amd64-release-off', 'abcdef123456')
    with open(os.path.join(os.path.dirname(stamp_only), 'CMakeCache.txt'), 'w') as f:
        f.write('WWE2K14_X:BOOL=ON\nOTHER:BOOL=OFF\n')
    T('B3b a stamp-only exe reads its options from the CMakeCache.txt beside it', lambda: C.exe_info(stamp_only)[0]['options'] == {'WWE2K14_X': 'ON'})
    T('B3b the other arm with different CMakeCache options is refused', lambda: 'options differ' in (C.exe_refusal(rec_of('A12'), stamp_only, 'abcdef1', dict(options={'WWE2K14_X': 'OFF'}))[0] or ''))
    nocache = fake_exe(os.path.join(tmp, 'e7'), 'win-amd64-release-off', 'abcdef123456')
    T('B3b an exe with no options anywhere next to an arm with options is refused', lambda: 'options' in (C.exe_refusal(
        rec_of('A12'), nocache, 'abcdef1', dict(options={'WWE2K14_X': 'ON'}))[0] or ''))
    nosave = os.path.join(tmp, 'bench_ns')
    os.makedirs(os.path.join(nosave, 'userdata_nosave', 'x'))
    with open(os.path.join(nosave, 'userdata_nosave', 'x', 'other.bin'), 'w') as f:
        f.write('x')
    nsc = C.Ctx(ns(runs_log=runs_log, bench=nosave))
    nsc.sdir = os.path.join(tmp, 'sdir_ns')
    T('B3c a mirror with no SaveData.Dat is REFUSED exit 3', lambda: C.preflight(nsc, rec_of('A12', lambda r: r['destination'].update(data='userdata_nosave')), 'before')[0] == 3)

    # ---- F1..F14
    T('F1 alone = true appends -Alone to the run_list line', lambda: ' -Alone' in C.launch_text(real_ctx, rec_of('A12', lambda r: r['run'].update(alone=True)), 'before', goodx, head)
      and ' -Alone' not in C.launch_text(real_ctx, rec_of('A12'), 'before', goodx, head))
    T('F2 a real run without --exe is REFUSED exit 3 (no crash)', lambda: rcmd('A12', arm='before', commit=head) == 3)
    T('F2 a real run without --commit is REFUSED exit 3', lambda: rcmd('A12', arm='before', exe=goodx) == 3)
    T('F3 a timed-out command is killed with its children (returns, flagged timed out)', lambda: (lambda r: r[1] and 'timed out' in r[0] or r[1])(
        C.run_cmd([sys.executable, '-c', 'import time; time.sleep(20)'], 3)))
    T('F4 lint refuses the flags the runner owns (settings file, launcher settings, audio_mute=false, vsync, mods dir, replay dir, game_data_root)', lambda: all(
        R.lint(rec_of('A12', lambda r, f=f: r['destination'].update(flags=f)), C.TREE)[0] for f in (
            '--wwe2k14_settings_file=x', '--wwe2k14_use_launcher_settings=true', '--audio_mute=false', '--vsync=true', '--wwe2k14_mods_dir=x',
            '--wwe2k14_replay_dir=x', '--game_data_root=x')))
    T('F4 lint refuses a switch the recipe repeats, and one the route header already sets (verbs.conflicts)', lambda: R.lint(rec_of('A12', lambda r: r['destination'].update(
        flags='--foo=1 --foo=2')), C.TREE)[0] and R.lint(rec_of('A12', lambda r: r['destination'].update(flags='--wwe2k14_fixed_seed=2')), C.TREE)[0])
    T('F4 lint: --wwe2k14_mods=true needs a mods tree', lambda: R.lint(rec_of('A12', lambda r: r['destination'].update(flags='--wwe2k14_mods=true')), C.TREE)[0])
    mdc = C.Ctx(ns(runs_log=runs_log, bench=os.path.join(tmp, 'bench_f5'), main=MAIN))
    mdc.sdir = os.path.join(tmp, 'sdir_f5')
    os.makedirs(mdc.bench)
    T('F5 preflight with make_mirror=False plants nothing and says it would', lambda: (lambda r: r[0] == 0 and any('would plant' in m for m in r[1]) and not os.path.isdir(
        os.path.join(mdc.bench, 'userdata_chk_A8')))(C.preflight(mdc, rec_of('A8'), 'before', make_mirror=False)))
    T('F6 --commit must be 7+ hex and a real commit', lambda: C.resolve_commit(C.TREE, 'abc')[1] and C.resolve_commit(C.TREE, 'deadbee1')[1]
      and C.resolve_commit(C.TREE, 'zzzzzzzz')[1] and C.resolve_commit(C.TREE, head[:9])[0] == head)
    f6 = mk_main('env_f6')

    def f6_rerun():
        stored(base_rec, 'before', 'FAULT', f6)
        return code_of(base_rec, 'before', f6) == 3
    T('F6 a FAULT BEFORE at the same commit + recipe is not re-run (exit 3)', f6_rerun)

    def f6_short():
        stored(base_rec, 'before', 'FAULT', f6, commit='a' * 40)
        stored(base_rec, 'after', 'PASS', f6)
        return code_of(base_rec, 'after', f6, head[:9]) == 3
    T('F6 a passing AFTER stands against the short sha of the same commit (full shas compared)', f6_short)
    f7log = os.path.join(tmp, 'f7.log')
    # --main envm on every call: the old code would run the real run_list (a stuck queue entry, 2026-11-21 11:47); here it only runs a stub
    T('F7 --runs-log / --bench / --main on a real run are REFUSED exit 3, nothing written', lambda: all(
        C.main(['run', 'A12', '--arm', 'before', '--exe', goodx, '--commit', head, '--main', envm, '--runs-log', f7log] + x) == 3 for x in (
            [], ['--bench', real_ctx.bench])) and not os.path.exists(f7log))
    T('F10 lint: a set bar on a count readout, and a number bar on a set guard, are errors', lambda: R.lint(rec_of('A12', lambda r: r['readout'][0].update(
        fixed='== {1,2}')), C.TREE)[0] and R.lint(rec_of('IMP', lambda r: [x.update(need='>= 1') for x in r['guard'] if x['agg'] == 'set']), C.TREE)[0])
    rt11 = os.path.join(tmp, 'route11.txt')
    with open(rt11, 'w') as f:
        f.write('# note only: --wwe2k14_launch_participants=1:2,3\n# --wwe2k14_match=x:p=1:2\nlog bench: start\n')
    T('F11 participants() reads only `# harness: flags` lines, not any comment', lambda: R.participants(rec_of('PLATE', lambda r: r['destination'].update(route=rt11)), MAIN, C.TREE)[0] is None
      and R.participants(rec_of('PLATE', lambda r: r['destination'].update(route=rt11, flags='--wwe2k14_launch_participants=1:2,3')), MAIN, C.TREE)[0] == {1, 3})
    T('F12 a == 0 count fault readout without a sibling guard is a lint error; A5d as shipped is clean', lambda: any('sibling' in e for e in R.lint(
        rec_of('A5d', lambda r: r.update(guard=[g for g in r['guard'] if 'key' not in g['name']])), C.TREE)[0])
        and not any('sibling' in e for e in R.lint(rec_of('A5d'), C.TREE)[0]))
    T('F12 A5d BEFORE on log 109 with every Cena key line removed = VOID:guard, not FAULT', lambda: verdict('A5d', 'before', [l for l in L[109] if 'key model_key:139' not in l]) == 'VOID:guard')
    T('F13 a run_list ABORT after the pending line is verdict REFUSED', lambda: C.parse_run_list('ABORT: no exe')[1] == 'abort'
      and RO.arm_verdict(rec_of('A12'), 'before', dict(guards=[], readouts=[], shots=[]), None, 0, 'abort') == 'REFUSED')
    rl2 = os.path.join(tmp, 'rl2.log')
    with open(rl2, 'w') as f:
        f.write('2026-01-01 00:00:00 row=A12 arm=BEFORE run=pending verdict=pending question=q\r\n2026-01-01 00:00:01 row=A12 arm=BEFORE run=pending verdict=REFUSED question=q\r\n')
    T('F13 runs_for skips REFUSED lines (the board counts runs only)', lambda: C.runs_for(rl2, 'A12') == [])
    rt14 = os.path.join(tmp, 'route14.txt')
    with open(rt14, 'w') as f:
        f.write('on_fail continue\nlog bench: start\n')
    T('F14 lint WARNs when the route says on_fail continue', lambda: any('on_fail continue' in w for w in R.lint(rec_of('A12', lambda r: r['destination'].update(route=rt14)), C.TREE)[1]))
    T('F14 the champion expansion adds plate:title only when the recipe says plate = true', lambda: 'plate:title=1' not in R.expand(
        rec_of('A12', lambda r: r['destination'].update(champion={'title': 1, 'char': 322})))[0]
        and 'plate:title=1' in R.expand(rec_of('A12', lambda r: r['destination'].update(champion={'title': 1, 'char': 322, 'plate': True})))[0])


if __name__ == '__main__':
    sys.exit(main())
