"""S101 lane 161: offline checks of the situation director's first verbs (passive / momentum / stock / passive_cpu). No game runs.

    py -I tools/director/director_selftest.py             the unit test (src/situation_director_core.h COMPILED with clang++ against a fake game), its planted twins,
                                                          the parse rule against the strings the goto resolver builds, the goto ops / flags, the wiring pins,
                                                          the idle-pin detector on run 7092 (must flag) and run 5910 (must not)
    py -I tools/director/director_selftest.py --listing   pins of every address / offset the director calls against the generated listing of the game's functions
                                                          (reads generated\\ of the main checkout, never printed)

Exit 0 = all passed. A check that cannot read its input fails loudly.
"""
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, 'tools', 'goto'))
import idle_pin_detector as D  # noqa: E402
import resolve  # noqa: E402

problems = []


def check(ok, msg):
    if not ok:
        problems.append(msg)
        print('  FAIL: ' + msg)


def find_clang():
    for c in (shutil.which('clang++'), r'C:\Program Files\LLVM\bin\clang++.exe', '/usr/bin/clang++'):
        if c and os.path.isfile(c):
            return c
    raise RuntimeError('no clang++ found (tools\\env.ps1 puts the project\'s LLVM on PATH): the director unit test cannot run')


def build(header_text, tmp, tag):
    """Compiles director_test.cpp against `header_text` as src/situation_director_core.h (a copy under tmp: a planted twin never touches the tree)."""
    inc = os.path.join(tmp, tag)
    os.makedirs(inc, exist_ok=True)
    with open(os.path.join(inc, 'situation_director_core.h'), 'w', encoding='utf-8') as f:
        f.write(header_text)
    exe = os.path.join(inc, 'director_test.exe')
    r = subprocess.run([find_clang(), '-std=c++17', '-I', inc, os.path.join(HERE, 'director_test.cpp'), '-o', exe], capture_output=True, text=True, timeout=180)
    if r.returncode != 0 or not os.path.isfile(exe):
        raise RuntimeError('director_test did not compile (%s): %s' % (tag, (r.stdout + r.stderr)[-600:]))
    return exe


def run_test(exe):
    r = subprocess.run([exe], capture_output=True, text=True, timeout=60)
    return r.returncode, r.stdout


# ---- the planted twins: (name, regex pattern, replacement); each must change the header exactly once and make the test FAIL ----
PLANTS = [
    ('wrong_offset_cpu_word', r'kComActiveOff = 100;', 'kComActiveOff = 104;'),
    ('wrong_offset_meter', r'kMeterOff = 76;', 'kMeterOff = 80;'),
    ('wrong_offset_stock', r'kStockOff = 1928;', 'kStockOff = 1932;'),
    ('wrong_offset_chp_data', r'kChpDataOff = 24;', 'kChpDataOff = 28;'),
    ('missing_slot_table_guard', r'  if \(e\.U32\(ch \+ kSlotOff\) != uint32_t\(slot\)\) return [^\n]*\n', '\n'),
    ('missing_referee_guard', r'  if \(role == 4 \|\| role == 5 \|\| role == 6 \|\| role == 10\) return [^\n]*\n', '\n'),
    ('missing_match_guard', r'if \(!gate\.empty\(\)\) \{', 'if (false) {'),
    ('missing_decided_guard', r'if \(g\.g472\) return', 'if (false) return'),
    ('float_not_in_f1', r'e\.Call\(Fn::SetMeter, data, 0, fraction\);', 'e.Call(Fn::SetMeter, data, 0, 0.0);'),
    ('percent_not_scaled', r'double\(pct\) \* 0\.01', 'double(pct)'),
    ('no_readback_check_passive', r'if \(now != want\) return [^\n]*\n', '\n'),
    ('pad_call_present', r'  // B1 \(lane 161 review\): NO pad-record call\.', '  e.Call(Fn::SetComActive, ch, 0, 0.0);  // the planted B1 fault (a call on the character)\n  // B1 (lane 161 review): NO pad-record call.'),
    ('no_reset_before_the_bell', r'g\.seq != kSeqInGame \|\| !g\.clock_started\) \{\n    const bool latch', 'g.seq != kSeqInGame) {\n    const bool latch'),
    ('retry_never_dropped', r'return saw_match && seq != kSeqInGame;', 'return false;'),
    ('no_state_reset_outside_match', r'    st = State\{\};\n', '\n'),
    ('human_passive_allowed', r'if \(was == 0\) return "refused: slot " \+ std::to_string\(slot\) \+ " is not CPU-driven', 'if (false) return "refused: slot " + std::to_string(slot) + " is not CPU-driven'),
    ('off_ignores_changed_word', r'    if \(was != 0\) \{  // S1', '    if (false) {  // S1'),
    ('off_needs_full_gate', r'const std::string gate = restore \? \(g\.seq == kSeqInGame \? std::string\(\) : GateVerdict\(g\)\) : GateVerdict\(g\);', 'const std::string gate = GateVerdict(g);'),
    ('team_shared_momentum_not_refused', r'  if \(e\.Call\(Fn::TeamShared, 0, 0, 0\.0\) != 0\) return "refused: team-shared momentum', '  if (false) return "refused: team-shared momentum'),
    ('team_shared_stock_not_refused', r'  if \(e\.Call\(Fn::TeamShared, 0, 0, 0\.0\) != 0\) return "refused: team-shared stock', '  if (false) return "refused: team-shared stock'),
    ('off_form_too_wide', r'inline bool IsOffForm\(const Cmd& c\) \{ return \(c\.verb == Verb::Passive && !c\.on\)', 'inline bool IsOffForm(const Cmd& c) { return (c.verb == Verb::Passive)'),
    ('off_without_ownership', r'    if \(!\(sv\.on && sv\.ch == ch\)\) return "refused: slot "', '    if (false) return "refused: slot "'),
    ('auto_every_tick', r'  a\.applied = true;\n', '\n'),
    ('stock_wrong_direction', r'for \(int i = was; i < target; \+\+i\) e\.Call\(Fn::AddStock', 'for (int i = was; i < target; ++i) e.Call(Fn::RemoveStock'),
    ('all_includes_humans', r'if \(e\.U32\(comp \+ kComActiveOff\) == 0 && !\(st\.s\[slot\]\.on && st\.s\[slot\]\.ch == ch\)\) \{', 'if (false) {'),
]


def unit_tests(tmp):
    header = open(os.path.join(REPO, 'src', 'situation_director_core.h'), encoding='utf-8').read()
    code, out = run_test(build(header, tmp, 'honest'))
    m = re.search(r'SUMMARY (\d+) passed, (\d+) failed', out)
    check(code == 0 and m and m.group(2) == '0', 'the unit test fails on the shipped header:\n' + out[-800:])
    n = int(m.group(1)) if m else 0
    print('  unit test: %s' % (m.group(0) if m else 'no summary'))
    caught = 0
    for name, pat, rep in PLANTS:
        hits = len(re.findall(pat, header))
        if hits != 1:
            check(False, 'plant %s: the pattern matches %d times in the header (must be exactly 1: the plant is stale)' % (name, hits))
            continue
        twin = re.sub(pat, lambda _m: rep, header, count=1)
        try:
            code, out = run_test(build(twin, tmp, 'plant_' + name))
        except RuntimeError as x:
            check(False, 'plant %s does not compile: %s' % (name, x))
            continue
        failed = [ln.split()[1] for ln in out.splitlines() if ln.startswith('FAIL')]
        if code == 0:
            check(False, 'plant %s was NOT caught by the unit test' % name)
        else:
            caught += 1
            print('  plant %-28s caught by: %s' % (name, ', '.join(failed[:3]) or 'a crash (exit %d)' % code))
    print('  planted twins caught: %d of %d' % (caught, len(PLANTS)))
    return n


def parse_rule(tmp):
    """The strings the goto resolver builds (director_arg) and the input-script spelling are accepted by the game's parse rule; garbage is not."""
    exe = build(open(os.path.join(REPO, 'src', 'situation_director_core.h'), encoding='utf-8').read(), tmp, 'parse')
    steps = [{'op': 'passive', 'slot': 1, 'on': True}, {'op': 'passive', 'slot': 'all', 'on': False}, {'op': 'momentum', 'slot': 2, 'pct': 100},
             {'op': 'momentum', 'slot': 0, 'pct': 0}, {'op': 'stock', 'slot': 3, 'n': 9}, {'op': 'passive_cpu', 'on': True}, {'op': 'passive_cpu', 'on': False}]
    texts = [resolve.director_arg(s) for s in steps]
    r = subprocess.run([exe, 'parse'], input='\n'.join(texts) + '\n', capture_output=True, text=True, timeout=30)
    got = r.stdout.splitlines()
    check(len(got) == len(texts) and all(g.startswith('OK') for g in got), 'the resolver\'s director strings are not all accepted by the game parse: %s -> %s' % (texts, got))
    for s, t in zip(steps, texts):   # the round trip text -> step -> text
        check(resolve.director_arg(resolve.director_step_from_text(t)) == t, 'round trip of %r' % t)
    bad = ['passive', 'passive 9 on', 'momentum 1 101', 'stock 1 10', 'passive_cpu', 'dance 1 2']
    r = subprocess.run([exe, 'parse'], input='\n'.join(bad) + '\n', capture_output=True, text=True, timeout=30)
    check(r.stdout.split() == ['ERR'] * len(bad), 'bad director strings are not all refused by the game parse: %s' % r.stdout.split())
    for t in bad:
        try:
            resolve.director_step_from_text(t) if t.split()[0] in resolve.DIRECTOR_OPS else (_ for _ in ()).throw(resolve.AddressError('x'))
            if t not in ('passive 9 on', 'momentum 1 101', 'stock 1 10'):   # these three parse as a step; the game refuses them (checked above)
                check(False, 'the resolver accepted %r' % t)
        except resolve.AddressError:
            pass


def goto_ops():
    plan = resolve.Plan('x', 'plan', steps=[{'op': 'nav', 'target': 'mode:wm30'}, {'op': 'wm30_arm', 'n': 3, 'code': 0, 'era': 0}, {'op': 'expect_log', 'text': 'jump', 'timeout_s': 9}])
    resolve.add_director(plan, ['momentum 0 100', 'passive all on'], passive_cpu=True)
    ops = [s['op'] for s in plan.steps]
    check(ops == ['nav', 'passive_cpu', 'wm30_arm', 'expect_log', 'momentum', 'passive'], 'plan ops are %s' % ops)
    for s in plan.steps:
        check(bool(resolve.describe_step(s)), 'describe_step of %s' % s['op'])
    plan2 = resolve.Plan('x', 'plan', steps=[{'op': 'nav', 'target': 't'}])
    resolve.add_director(plan2, passive_cpu=True)
    check([s['op'] for s in plan2.steps] == ['nav', 'passive_cpu'], 'a plan with no match step gets the latch at its end: %s' % plan2.steps)
    plan3 = resolve.Plan('x', 'error', reason='no')
    resolve.add_director(plan3, ['stock 0 1'], True)
    check(plan3.steps == [], 'a plan that is not a plan is left alone')
    try:
        resolve.add_director(resolve.Plan('x', 'plan'), ['stock 0'])
        check(False, 'a bad --director text was accepted')
    except resolve.AddressError:
        pass


def rd(rel):
    return open(os.path.join(REPO, *rel.split('/')), encoding='utf-8').read()


def wiring():
    live, sd, app, inp, glue, core = rd('src/live_control.h'), rd('src/scene_director.h'), rd('src/wwe2k14_app.h'), rd('src/input_script.h'), rd('src/situation_director.h'), rd('src/situation_director_core.h')
    for ok, msg in [
        ('g_director_live' in live and 'director_cmd' in live and '"director"' in live, 'live_control.h does not route !director'),
        ('g_situation_tick(ctx, base)' in sd and '(*g_situation_tick)(PPCContext&, uint8_t*) = nullptr' in sd, 'scene_director.h does not call the director tick'),
        ('uint32_t withf(' in sd and 'ctx.f1.f64 = f1;' in sd, 'scene_director.h Caller has no float call (withf / ctx.f1)'),
        ('#include "situation_director.h"' in app, 'wwe2k14_app.h does not include situation_director.h'),
        ('Op::kDirector' in inp and 'situation::Submit(step.text)' in inp and 'situation::IsVerb(cmd)' in inp, 'input_script.h does not carry the director verbs'),
        ('call.withf(sub_825F1F10, {r3}, f1)' in glue, 'the momentum setter is not called with f1'),
        ('call(sub_823C8FB8, {r3, r4})' in glue and 'sub_8241DAB0' not in glue.replace('B1', '') .replace('lane 161 review B1', '') and 'call(sub_82C31CB0, {r3, r4})' in glue and 'call(sub_825E7168, {})' in glue, 'a leaf call in the glue changed'),
        ('call(sub_825F4510, {r3})' in glue and 'call(sub_825F1DF0, {r3})' in glue, 'a stock call in the glue changed'),
        ('g_director_live = &LiveCommand' in glue and 'g_situation_tick = &Tick' in glue, 'the glue does not register itself'),
        ('GateVerdict(g)' in core and 'const Gate g = e.GetGate();' in core, 'Run does not test the match gate'),
        ('src/situation_director_core.h' in rd('tools/director/director_selftest.py') or True, ''),
    ]:
        check(ok, msg)
    # nothing runs unless asked: the tick is free when idle (the latch is off, the queue empty, nothing pending)
    check('if (g_state.passive_cpu || g_auto.applied)' in glue and 'TakeQueued(text)' in glue and 'g_pending.exchange(false)' in glue, 'the tick is not a no-op when idle')
    check('Housekeeping(g_state, g)' in glue and 'seq != scenario::kSeqInGame' in glue, 'the glue does not reset the state outside the in-game sequence on every tick (S1)')
    check('replay::verbs::Playing() && !IsOffForm(c)' in glue and 'g_record_event(scenario::Frame(), "director " + text)' in glue and 'g_event_handlers.push_back(&OnReplayEvent)' in glue, 'the replay contract is not wired (S3)')
    check('DropsRetry(r.saw_match, seq)' in glue and 'playback diverges' in glue, 'F3 / F4 not wired in the glue')
    check('NOT DONE after' in glue and 'kScriptBusyFrames' in glue, 'a script verb can still be dropped silently (S6)')
    check('Fn::PadRecord' not in core and 'Fn::PadRecord' not in glue and 'PadRecord' not in glue, 'a pad-record call is back in the director (B1)')
    # the script verbs: a line's text is checked at parse time (a bad line fails the script before the game starts)
    check('director::Parse' not in inp, 'input_script.h still names the old namespace (wwe2k14::director is the match director)')


def detector():
    fx = os.path.join(HERE, 'fixtures')
    r_j3 = D.analyse(D.read_events(os.path.join(fx, 'run7092_match.jsonl'))[0])
    r_ok = D.analyse(D.read_events(os.path.join(fx, 'run5910_match.jsonl'))[0])
    check(len(r_j3) == 1 and r_j3[0]['verdict'] == 'no_defender' and r_j3[0]['pinner'] == 1 and r_j3[0]['loser'] == 0, 'fixture run 7092 (J3) is not no_defender as the CPU pinning an idle slot 0: %s' % r_j3)
    check(len(r_ok) == 1 and r_ok[0]['verdict'] == 'clean', 'fixture run 5910 is flagged: %s' % r_ok)
    # a passive-by-director pin is reported expected, not a flag; a move by the loser before the pin is a player
    ev = D.read_events(os.path.join(fx, 'run7092_match.jsonl'))[0]
    check(D.analyse(ev, [{'slot': 0, 'on': True, 'after_frame': 19000, 'line': 1}])[0]['verdict'] == 'passive', 'a passive-by-director pin is not reported as passive')
    check(D.analyse(ev, [{'slot': 0, 'on': True, 'after_frame': 19200, 'line': 1}])[0]['verdict'] == 'no_defender', 'a passive line AFTER the pin must not explain it')
    check(D.analyse(ev, [{'slot': 0, 'on': True, 'after_frame': 18000, 'line': 1}, {'slot': 0, 'on': False, 'after_frame': 18500, 'line': 2}])[0]['verdict'] == 'no_defender', 'a passive off before the pin must end the explanation')
    check(D.analyse(ev, [{'slot': 0, 'on': True, 'after_frame': None, 'line': 1}])[0]['verdict'] == 'passive', 'a passive line before any bus line (frame unknown) is before the pin')
    mv = ev + [{'f': 18000, 'kind': 'move', 'slot': 0, 'class': 'strike'}]
    check(D.analyse(mv)[0]['verdict'] == 'clean', 'a move by the loser before the pin is not accepted as a sign of a player')
    mv_far = ev + [{'f': 1000, 'kind': 'move', 'slot': 0, 'class': 'strike'}]
    check(D.analyse(mv_far)[0]['verdict'] == 'no_defender', 'a move 18000 frames before the pin must be outside the window')
    check(D.analyse([e for e in ev if e.get('what') != 'finish' or e.get('finish_type') != 1])[0]['verdict'] == 'unfinished', 'a pin with no finish is not reported unfinished')
    # the real run logs, when they still exist (the disk tidies itself after 24 h: the fixtures above carry the same lines)
    real = glob.glob(r'C:\Users\Jarrod\wwe2k14-recomp\out\build\win-amd64-release\logs\events_7092.jsonl')
    real5910 = glob.glob(r'C:\Users\Jarrod\wwe2k14-recomp\out\build\win-amd64-release\logs\events_5910.jsonl')
    for path, want in [(p, 'no_defender') for p in real] + [(p, 'clean') for p in real5910]:
        res = D.analyse(D.read_events(path)[0])
        check(any(x['verdict'] == want for x in res), 'real %s: no %s verdict: %s' % (os.path.basename(path), want, [x['verdict'] for x in res]))
        print('  real %s: %s' % (os.path.basename(path), [x['verdict'] for x in res]))
    if not real or not real5910:
        print('  (real event files %s gone: the committed fixtures were used)' % ('7092' if not real else '5910'))
    # the command line: exit 3 flagged, 0 clean
    for fixture, code in (('run7092_match.jsonl', 3), ('run5910_match.jsonl', 0)):
        r = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'idle_pin_detector.py'), os.path.join(fx, fixture)], capture_output=True, text=True, timeout=30)
        check(r.returncode == code, 'detector exit for %s is %d, not %d: %s' % (fixture, r.returncode, code, r.stdout[-200:]))


LISTING = {   # address: [text that must be in the listing]
    '827349E8': ['lwz r3,3088(r3)', 'cntlzw r11,r31', 'bl 0x823c8fb8'],
    '823C8FB8': ['stw r4,100(r3)'],
    '82735298': ['lwz r3,3728(r3)', 'lis r4,17224', 'ori r4,r4,20512', 'bl 0x82c31cb0', 'lwz r3,24(r3)', 'lfs f0,15500(r10)', 'bl 0x825f1f10'],
    '827354A0': ['lwz r30,24(r3)', 'bl 0x825f4510', 'bl 0x825f1df0'],
    '825F1F10': ['fmr f31,f1', 'stfs f31,76(r31)', 'lwz r10,3720(r11)'],
    '825F4510': ['lwz r11,32(r31)', 'addi r11,r11,1816', 'lwz r10,112(r11)', 'stw r10,112(r11)'],
    '825F1DF0': ['addi r11,r11,1816', 'lwz r10,112(r11)', 'addi r10,r10,-1', 'stw r10,112(r11)'],
    '825F1C50': ['lwz r3,1928(r11)'],
    '8241DAB0': ['lwz r3,132(r3)', 'stw r30,1888(r31)'],
    '823AE0B8': ['cmpwi cr6,r3,9', 'lwzx r3,r10,r9'],
    '825A0B50': ['lwz r3,3088(r31)', 'lwz r3,100(r3)'],
}


def listing():
    main = os.environ.get('RDR_MAIN_CHECKOUT') or (REPO if os.path.isdir(os.path.join(REPO, 'generated', 'default')) else os.path.abspath(os.path.join(REPO, '..', '..', '..')))
    files = glob.glob(os.path.join(main, 'generated', 'default', 'wwe2k14_recomp.*.cpp'))
    if not files:
        check(False, '--listing: no generated code under %s (this check needs the player\'s own recompiled output)' % main)
        return
    for addr, needles in LISTING.items():
        out = subprocess.run([sys.executable, '-I', os.path.join(main, 'tools', 'analysis', 'list_function.py'), addr], cwd=main, capture_output=True, text=True, timeout=120).stdout
        low = out.lower()
        if 'not found' in low:
            check(False, '--listing: sub_%s not in the generated code' % addr)
            continue
        for n in needles:
            check(n.lower() in low, '--listing: sub_%s no longer has "%s"' % (addr, n))
    # the numbers in the core, against the listing's
    core = rd('src/situation_director_core.h')
    for name, val in (('kComOff', 3088), ('kComActiveOff', 100), ('kListOff', 3728), ('kChpDataOff', 24), ('kMeterOff', 76), ('kStockOff', 1928), ('kChpTag', 0x43485020)):
        m = re.search(r'%s = (0x[0-9A-Fa-f]+|\d+);' % name, core)
        check(bool(m) and int(m.group(1), 0) == val, 'core %s is not %s' % (name, val))
    check(0x4348 == 17224 and (0x4348 << 16 | 20512) == 0x43485020, 'the CHP tag arithmetic (lis 17224 / ori 20512)')
    print('  listing pins: %d functions, %d texts' % (len(LISTING), sum(len(v) for v in LISTING.values())))


def main(argv):
    tmp = tempfile.mkdtemp(prefix='l161_')
    try:
        if '--listing' in argv:
            listing()
        else:
            n = unit_tests(tmp)
            parse_rule(tmp)
            goto_ops()
            wiring()
            detector()
            print('  unit checks run: %d' % n)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if problems:
        print('FAILED: %d problem(s)' % len(problems))
        return 1
    print('PASSED')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
