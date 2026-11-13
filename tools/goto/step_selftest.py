"""S101 lane 121: offline checks of `wm30:N@step:K` / `wm30:N@moment:BANK` / `!wm30 step K`. No game runs.

    py -I tools/goto/step_selftest.py              the address, the resolver (synthetic step table), the game-side parse rule and guard truth table
                                                   (src/wm30_step_args.h COMPILED with clang++ and driven), wiring pins, planted twins
    py -I tools/goto/step_selftest.py --table      the resolver over the REAL step table (reads the player's pacs): every one of lane R1's 50 plays
                                                   resolves to the step R1 says plays that bank; the step table's entry rule re-derived; planted twins
    py -I tools/goto/step_selftest.py --listing    pins of every address / offset src/wm30_step.h uses against the generated listing of the game's
                                                   step setter (sub_82731D80) and the generic variable setter / getter (reads generated/, never printed)

Exit 0 = all passed. A check that cannot read its input fails loudly.
"""
import itertools
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import address  # noqa: E402
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
    raise RuntimeError('no clang++ found (tools\\env.ps1 puts the project\'s LLVM on PATH): the step parse / truth table test cannot run')


def build_test(header_text, tmp, tag):
    """Compiles tools/goto/step_test.cpp against `header_text` as src/wm30_step_args.h (a copy under tmp, so a planted twin never touches the tree)."""
    inc = os.path.join(tmp, tag)
    os.makedirs(inc, exist_ok=True)
    with open(os.path.join(inc, 'wm30_step_args.h'), 'w', encoding='utf-8') as f:
        f.write(header_text)
    exe = os.path.join(inc, 'step_test.exe')
    r = subprocess.run([find_clang(), '-std=c++17', '-I', inc, os.path.join(HERE, 'step_test.cpp'), '-o', exe], capture_output=True, text=True, timeout=180)
    if r.returncode != 0 or not os.path.isfile(exe):
        raise RuntimeError('step_test did not compile: %s' % (r.stdout + r.stderr)[-600:])
    return exe


def run_lines(exe, lines):
    r = subprocess.run([exe], input='\n'.join(lines) + '\n', capture_output=True, text=True, timeout=60)
    out = r.stdout.split('\n')
    if out and out[-1] == '':
        out.pop()
    if len(out) != len(lines):
        raise RuntimeError('step_test answered %d lines for %d inputs' % (len(out), len(lines)))
    return out


# ---- the truth table, written out independently of the header (the spec) ----
def expected(started, in_game, clock, obj, g472, g468, lg, il, ef, force, step, sz, held=0):
    if not started:
        return ('refused', 'not in a 30 Years match')
    if not in_game:
        return ('busy', 'still loading')
    if not clock:
        return ('busy', 'clock has not started')
    if g472:
        return ('refused', 'already ended')
    if g468:
        return ('refused', 'a scene is already running')
    if held >= 10:  # lane 144: the match clock is held (J3's paused match)
        return ('busy', 'the match clock is held at 15')
    if not obj:
        return ('busy', 'script object does not exist')
    if sz:
        return ('busy', 'still in step 0')
    if lg and not il:
        return ('refused', 'not a step of this match')
    if step == 0 and not force:
        return ('refused', 'step 0 is the script')
    if not lg and not force:
        return ('refused', 'no steps= list')
    if not ef and not force:
        return ('refused', 'entry condition')
    return None


def truth_cases():
    cases = []
    for st, ig, ck, ob, f472, f468, lg_il, ef, fo, k, sz, held in itertools.product((0, 1), (0, 1), (0, 1), (0, 1), (0, 7), (0, 1, 2), ((0, 0), (1, 0), (1, 1)), (0, 1), (0, 1), (0, 2), (0, 1), (0, 9, 10, 50)):  # lane 144: clock_held
        lg, il = lg_il
        cases.append(('verdict %d %d %d %d %d %d %d %d %d %d %d %d 15 %d' % (st, ig, ck, ob, f472, f468, lg, il, ef, fo, k, sz, held), expected(st, ig, ck, ob, f472, f468, lg, il, ef, fo, k, sz, held)))
    return cases


def check_truth(exe, cases):
    out = run_lines(exe, [c[0] for c in cases])
    bad = []
    for (text, want), got in zip(cases, out):
        if want is None:
            if got != 'GO':
                bad.append('%s: want GO, got "%s"' % (text, got[:70]))
        else:
            if got == 'GO' or not got.startswith(want[0] + ':') or want[1] not in got:
                bad.append('%s: want %s "%s", got "%s"' % (text, want[0], want[1], got[:70]))
    return bad


PARSE_GOOD = [('step 2', (2, 0, 0, 0, '-')), ('step 2 force', (2, 1, 0, 0, '-')), ('  step   0  ', (0, 0, 0, 0, '-')), ('step 999', (999, 0, 0, 0, '-')),
              ('step 3 entry=none', (3, 0, 0, 1, '-')), ('step 3 steps=0,1,2,3', (3, 0, 1, 0, '0,1,2,3')), ('step 7 steps=7 entry=none force', (7, 1, 1, 1, '7')),
              ('step 7 force entry=none steps=0,7', (7, 1, 1, 1, '0,7'))]
PARSE_BAD = ['step', 'step x', 'step -1', 'step +2', 'step 1000', 'step 2x', 'step 2 force force', 'step 2 entry=none entry=none', 'step 2 steps=1 steps=2', 'step 2 steps=',
             'step 2 steps=1,', 'step 2 steps=,1', 'step 2 steps=1,,2', 'step 2 steps=a', 'step 2 steps=1000', 'step 2 entry=some', 'step 2 Force', 'step 2 forced', 'step 2 3',
             'step 2 force entry=none steps=1 x', 'steps 2', 'fire 5', '2', 'off', '', 'step 2 steps=' + ','.join(['1'] * 65)]


def parse_cases():
    # the strings the resolver's executor builds, over a spread of steps / lists / flags
    good = []
    for k in (0, 2, 11, 999):
        for fo in (False, True):
            for ls in ((), (0, 2, 11)):
                for ef in (False, True):
                    good.append((resolve.step_arg(k, fo, ls, ef), (k, int(fo), int(bool(ls)), int(ef), ','.join(str(x) for x in ls) or '-')))
    good += PARSE_GOOD
    return [('parse ' + t, ('STEP',) + tuple(v)) for t, v in good] + [('parse ' + t, ('ERR',)) for t in PARSE_BAD]


def check_parse(exe, cases):
    out = run_lines(exe, [c[0] for c in cases])
    bad = []
    for (text, want), got in zip(cases, out):
        g = got.split()
        have = ('STEP', int(g[1]), int(g[2]), int(g[3]), int(g[4]), g[5]) if g[0] == 'STEP' else (g[0],)
        if have != want:
            bad.append('%r: want %s, got %s' % (text, want, got[:60]))
    return bad


ROUTE = [('step 5', 1), ('  step', 1), ('5', 0), ('off', 0), ('', 0), ('stepx 5', 0), ('fire 5', 0), ('5 10101', 0)]


def check_route(exe):
    out = run_lines(exe, ['route ' + t for t, _ in ROUTE])
    return ['route %r: want %d, got %s' % (t, w, o) for (t, w), o in zip(ROUTE, out) if o != 'ROUTE %d' % w]


# ---- the address and the resolver (synthetic scex + step table: no game files) ----
def step(free, banks=(), entries=None):
    return dict(free=free, entries=[[]] if free else [['dmgLevel(0, -1) >= 2'], ['inZone(0, 0) != 0']] if entries is None else entries, banks=list(banks))


def synthetic_ctx(table=None):
    scex = {'eras': {0: [10300, 10301, 10302], 1: [], 2: [], 3: [], 4: []},
            'matches': {3: {'era': 0, 'code': 10300, 'record': 3}, 7: {'era': 0, 'code': 10300, 'record': 7}, 0: {'era': 0, 'code': 10000, 'record': 0}}}
    if table is None:
        table = {3: dict(steps={0: step(True), 1: step(False), 2: step(False, [1090]), 3: step(False), 10: step(False)}, plays={1090: 2}),
                 0: dict(steps={0: step(True), 3: step(True, [1010])}, plays={1010: 3}),
                 7: dict(steps={0: step(True)}, plays={})}
    return resolve.Context(scex=scex, step_table=table)


def step_of(plan):
    s = [x for x in plan.steps if x['op'] == 'wm30_step']
    return s[0] if len(s) == 1 else None


def check_address_and_resolver():
    for t in ('wm30:3@step:2', 'wm30:3@step:2/force', 'wm30:45@step:999', 'wm30:3@moment:1090', 'wm30:3@moment:9999', 'wm30:3@moment:1000'):
        a = address.parse(t)
        check(str(a) == t and (a.step >= 0 or a.moment_bank >= 0), 'address %s did not round-trip (%s)' % (t, a))
    for t in ('wm30:3@step:', 'wm30:3@step:abc', 'wm30:3@step:1000', 'wm30:3@step:2/forced', 'wm30:3@step:-1', 'wm30:3@moment:999', 'wm30:3@moment:7', 'wm30:3@moment:1090/force',
              'wm30:3@moment:1090@step:2', 'wm30:3@step:2@fire:1090', 'wm30:3@moment:1@step:2', 'wm30:3@moment:10301@step:2', 'wm30:3@moment:1090@fire:1090', 'wm30:46@step:2',
              'match:Cena vs Orton@step:2'):
        try:
            address.parse(t)
            check(False, 'address %s should be refused' % t)
        except address.AddressError:
            pass
    # old forms still parse as they did
    for t in ('wm30:3', 'wm30:3@moment:1', 'wm30:3@moment:10301', 'wm30:3@fire:1090/force'):
        check(str(address.parse(t)) == t, 'old address %s changed' % t)
    ctx = synthetic_ctx()
    p = resolve.resolve('wm30:3@moment:1090', ctx)
    check(p.status == 'plan', 'wm30:3@moment:1090 is %s (%s)' % (p.status, p.reason))
    ops = [s['op'] for s in p.steps]
    check(ops == ['nav', 'wm30_arm', 'expect_log', 'events_mark', 'wm30_to_bell', 'wait_event', 'wm30_clear_box', 'wm30_step', 'expect_fire', 'expect_fire', 'expect_fire', 'expect_fire'], 'moment plan steps are %s' % ops)
    s = step_of(p)
    check(s is not None and s['step'] == 2 and s['force'] and not s['entry_free'] and 2 in s['steps'], 'moment 1090 should plan step 2 with force (objective skipped): %s' % s)
    check(any(x['op'] == 'wait_event' and x['fields'] == {'what': 'clock_started'} for x in p.steps), 'the plan does not wait for the bell before the step')
    exp = [x for x in p.steps if x['op'] == 'expect_fire']
    check('25 -> 14 (frame' in exp[1]['any'] and '25 -> 19 (frame' in exp[1]['fail'] and '14 -> 10 (frame' in exp[2]['any'] and '14 -> 8 (frame' in exp[2]['any'] and '14 -> 21 (frame' in exp[2]['fail']
          and '14 -> 10 (frame' not in exp[2]['fail'] and 'back in the match (state 12)' in exp[3]['any'][0] and '14 -> 21 (frame' in exp[3]['fail']
          and any('CPU' in x and 'finish the match' in x for x in p.notes),  # lane 150
          'the expect_fire steps lost the 25 -> 14 / 14 -> 10 / back-in-the-match states or the route note')
    check(any('objective' in n or 'past the script' in n for n in p.notes), 'the moment plan does not say it skips the script\'s own test')
    check(resolve.resolve('wm30:3@moment:1100', ctx).status == 'error', 'a bank the chunk never plays was planned')
    check(resolve.resolve('wm30:7@moment:1090', ctx).status == 'error', 'a match with no scene accepted a bank')
    # @step: a guarded step needs /force, a free one does not, an unknown one is refused either way
    check(resolve.resolve('wm30:3@step:2', ctx).status == 'error', 'a guarded step was planned without /force')
    pf = resolve.resolve('wm30:3@step:2/force', ctx)
    check(pf.status == 'plan' and step_of(pf)['force'] and step_of(pf)['step'] == 2, '@step:2/force did not plan with force')
    pf0 = resolve.resolve('wm30:0@step:3', ctx)
    s0 = step_of(pf0)
    check(pf0.status == 'plan' and s0 and s0['entry_free'] and not s0['force'], 'a free step needs no force: %s' % (s0,))
    check(resolve.resolve('wm30:3@step:4', ctx).status == 'error', 'a step the chunk does not have was planned')
    check(resolve.resolve('wm30:3@step:4/force', ctx).status == 'error', '/force planned a step the chunk does not have')
    pn = resolve.resolve('wm30:3@step:1/force', ctx)
    check(pn.status == 'plan' and not any(x['op'] == 'expect_fire' for x in pn.steps), 'a step that plays no scene should not wait for the scene flow')
    check(resolve.resolve('wm30:3', ctx).steps == resolve.resolve('wm30:3', ctx).steps and all(x['op'] != 'wm30_step' for x in resolve.resolve('wm30:3', ctx).steps),
          'a plain wm30:N gained a step')
    # planted: a table that lies must be seen by the same comparisons
    shifted = synthetic_ctx({3: dict(steps={0: step(True), 1: step(False), 2: step(False, [1090]), 3: step(False), 10: step(False)}, plays={1090: 3}), 0: dict(steps={0: step(True), 3: step(True)}, plays={}), 7: dict(steps={0: step(True)}, plays={})})
    ps = step_of(resolve.resolve('wm30:3@moment:1090', shifted))
    check(ps is not None and ps['step'] != 2, 'planted: a table with the play shifted to step 3 still gave step 2 (the resolver ignores the table)')


def check_game_side():
    header = open(os.path.join(REPO, 'src', 'wm30_step_args.h'), encoding='utf-8').read()
    with tempfile.TemporaryDirectory() as tmp:
        exe = build_test(header, tmp, 'real')
        pc, tc = parse_cases(), truth_cases()
        bad = check_parse(exe, pc)
        check(not bad, 'the game parse rule disagrees: %s' % bad[:3])
        bad = check_route(exe)
        check(not bad, 'the !wm30 routing rule disagrees: %s' % bad[:3])
        bad = check_truth(exe, tc)
        check(not bad, 'the guard truth table disagrees (%d of %d): %s' % (len(bad), len(tc), bad[:3]))
        twins = {
            'clock guard dropped': ('if (!in.clock_started)', 'if (false && !in.clock_started)'),
            'entry-condition guard dropped': ('if (!in.entry_free && !in.force)', 'if (false)'),
            'step-list guard dropped (a wrong step index passes)': ('if (in.list_given && !in.in_list)', 'if (false)'),
            'no-list guard dropped': ('if (!in.list_given && !in.force)', 'if (false)'),
            'G+468 guard dropped': ('if (in.g468 != 0)', 'if (false && in.g468 != 0)'),
            'match-ended guard dropped': ('if (in.g472 != 0)', 'if (false && in.g472 != 0)'),
            'not-started guard dropped': ('if (!in.started)', 'if (false && !in.started)'),
            'loading guard dropped': ('if (!in.in_game)', 'if (false && !in.in_game)'),
            'step-0 busy guard dropped (267 == 0 passes)': ('if (in.shared_zero)', 'if (false && in.shared_zero)'),
            'step 0 allowed without force': ('if (in.step == 0 && !in.force)', 'if (false)'),
            'object guard dropped': ('if (!in.have_object)', 'if (false && !in.have_object)'),
            'clock-held guard dropped (lane 144)': ('if (in.clock_held >= kClockHeldFrames)', 'if (false && in.clock_held >= kClockHeldFrames)'),
            'clock-held threshold off by one (lane 144)': ('in.clock_held >= kClockHeldFrames', 'in.clock_held > kClockHeldFrames'),
            'entry guard ignores force': ('if (!in.entry_free && !in.force)', 'if (!in.entry_free)'),
            'duplicate force allowed in parse': ('if (out.force) return', 'if (false) return'),
            'step ceiling removed': ('Digits(w[1], 3)', 'Digits(w[1], 9)'),
            'list entries unchecked': ('if (!Digits(cur, 3)) return', 'if (false) return'),
        }
        for what, (old, new) in twins.items():
            if old not in header:
                check(False, 'planted twin "%s": anchor %r is gone from the header (the test lost its anchor)' % (what, old))
                continue
            tag = 'twin_' + re.sub(r'\W', '', what)
            exe2 = build_test(header.replace(old, new, 1), tmp, tag)
            try:
                caught = bool(check_truth(exe2, tc)) or bool(check_parse(exe2, pc))
            except RuntimeError:
                caught = True  # the twin crashed or answered short: it differs from the real header all the same
            check(caught, 'planted twin (%s) was not caught by the truth table / parse cases' % what)
    print('  game side: parse %d cases, routing %d, truth table %d combinations; %d planted twins caught' % (len(pc), len(ROUTE), len(tc), len(twins)))


def between(text, a, b):
    i = text.index(a)
    return text[i:text.index(b, i)]


def wiring_problems(step, fire, jump, app, forced):
    """The text pins of src/wm30_step.h and its seams; returns the problems (so a planted twin can be run through the same pins)."""
    bad = []

    def pin(ok, msg):
        if not ok:
            bad.append(msg)
    pin('call(sub_82A9CAD8, {uint32_t(kStepVar), uint32_t(a.step)})' in step and 'REX_EXTERN(sub_82A9CAD8)' in step, 'the step does not CALL the game\'s own commit sub_82A9CAD8(267, K)')
    pin('sub_82731D80' not in step.split('#pragma once')[1] and 'kFakeL' not in step.split('#pragma once')[1],
        'the step still uses the Lua native sub_82731D80 / the sentinel L (review: obj+3632 is only a next-step slot)')
    fn = between(step, 'inline std::string Step(', '// The game thread')
    pin('call(sub_82A9CAD8' in fn and fn.index('StepVerdict(in)') < fn.index('call(sub_82A9CAD8'), 'the guards must run before the game function is called')
    pin(fn.index('StepVerdict(in)') < fn.index('ScriptObject()'), 'the script object must be read only after the earlier guards')
    pin('in.clock_started = int32_t(U32(kClockWord)) > 0' in fn and 'kClockWord = 0x836F3928' in step, 'the clock guard lost its input (the event bus clock word 0x836F3928)')
    pin('in.shared_zero = !SharedVar(shared) || shared == 0' in fn, 'review F3: the step-0 busy input is gone')
    pin('step: set {} at frame {}' in fn, 'the "step: set K at frame F" log line is gone')
    pin('after_var != a.step' in fn and 'return "failed:' in fn and fn.index('return "failed:') < fn.index('g_watching = true'), 'review F4: a read-back != K must return failed before anything is watched')
    pin('g_watching = true' in fn and 'g_watch.store(true)' in fn, 'the step no longer starts lane 108\'s flow watch')
    pin('g_vw_active = true' in fn and 'step: watch +{} frames: variable 267' in step and 'kVarWatchFrames = 600' in step, 'review F4: the variable-267 / slot watch is gone')
    pin('kVarOwner = 0x839295F0' in step and 'kStepVar + 296) * 8u' in step, 'the shared-array read lost its address (B\' + (var + 296) * 8)')
    pin('g_step_fake_active' not in fire and 'g_step_var' not in fire, 'lane 108\'s hook still carries the step branch')
    pin('if (g_step_tick) g_step_tick(ctx, base);' in fire, 'wm30_fire.h Tick does not call the step tick')
    live = between(step, 'inline std::string LiveCommand', 'inline const bool g_registered')
    pin('U32(' not in live and 'Guest(' not in live, 'the step LiveCommand reads guest memory on the pipe thread')
    pin('IsStepCommand(args)' in jump and jump.index('IsStepCommand(args)') < jump.index('ParseArmArgs(args, a)'), 'wm30_jump.h parses the match number before routing step')
    pin('#include "wm30_step.h"' in app, 'wwe2k14_app.h does not include wm30_step.h')
    pin('in.clock_held = wwe2k14::wm30_fire::g_clk_held' in fn and 'in.clock = wwe2k14::wm30_fire::g_clk_held' not in fn, 'lane 144: the step verdict lost its clock-held input')
    pin('ClockTick(seq);' in fire and 'g_clk_held = (c > 0' in fire, 'lane 144: the per-frame tick no longer tracks the held clock')
    pin('script object {:08X}{}) clock {}' in fn and 'variable 267 = {} (was {}), slot = {} (was {}) clock {}' in step and 'step: {}", GateWords()' in step
        and '836F3938={:08X}' in step and '[[839295F0]+8]+164' in step and 'script object+16' in step, 'lane 144: the clock / gate-word log lines are gone')
    pin('kStep = 6' in forced and 'v == kStep ? "step"' in forced, 'scene_forced.h lost the step verb')
    return bad


def check_wiring():
    rd = lambda n: open(os.path.join(REPO, 'src', n), encoding='utf-8').read()
    texts = dict(step=rd('wm30_step.h'), fire=rd('wm30_fire.h'), jump=rd('wm30_jump.h'), app=rd('wwe2k14_app.h'), forced=rd('scene_forced.h'))
    for m in wiring_problems(**texts):
        check(False, m)
    cw = open(os.path.join(REPO, 'src', 'case', 'case_where.h'), encoding='utf-8').read()
    check('in.forced_verb == "step" || in.forced_verb == "fire"' in cw and '"@step:"' in cw and '"@fire:"' in cw, 'review F6: case_where.h does not map the step / fire verbs to wm30:N@step:K / @fire:B')
    # planted twins: each must be caught by the same pins
    twins = {
        'the commit call removed': ('call(sub_82A9CAD8, {uint32_t(kStepVar), uint32_t(a.step)});', '(void)0;'),
        'the read-back check removed': ('after_var != a.step', 'false'),
        'the step-0 busy input removed': ('in.shared_zero = !SharedVar(shared) || shared == 0', 'in.shared_zero = false'),
        'the variable watch removed': ('g_vw_active = true;\n  g_vw_start', 'g_vw_active = false;\n  g_vw_start'),
    }
    for what, (old, new) in twins.items():
        if old not in texts['step']:
            check(False, 'planted wiring twin "%s": anchor %r is gone (the test lost its anchor)' % (what, old))
            continue
        tw = dict(texts, step=texts['step'].replace(old, new, 1))
        check(bool(wiring_problems(**tw)), 'planted wiring twin (%s) was not caught' % what)
    print('  wiring pins ok; %d wiring twins caught' % len(twins))


# ---- the real table ----
def reach_free(steps):
    """Independent re-derivation of the free set from the edges ([[source step or None, [conds]], ...] per step): reachable from step 0 with no test."""
    free = {0}
    while True:
        new = {n for n, s in steps.items() if n not in free and any(src in free and not conds for src, conds in s['edges'])}
        if not new:
            return free
        free |= new


def table_main():
    sys.path.insert(0, os.path.join(REPO, 'tools', 'wm30'))
    import scene_table
    import step_table
    game = os.path.join(resolve.main_checkout(), 'game_data', 'extracted')
    if not os.path.isdir(game):
        raise RuntimeError('no game files at %s' % game)
    rows, missing = scene_table.build(game)
    truth = {r['index']: {int(s['bank']): int(s['step']) for s in r['scenes']} for r in rows if r['index'] is not None}  # lane R1's plays, an independent path
    check(len(truth) == 46 and sum(len(v) for v in truth.values()) == 50, 'lane R1\'s table is not 46 matches / 50 plays: %d / %d' % (len(truth), sum(len(v) for v in truth.values())))
    table = step_table.build(game)
    ctx = resolve.Context(step_table=table)
    n_plays = n_free = n_guarded = 0
    for n, plays in sorted(truth.items()):
        for bank, k in plays.items():
            p = resolve.resolve('wm30:%d@moment:%d' % (n, bank), ctx)
            s = step_of(p)
            check(p.status == 'plan' and s is not None and s['step'] == k and k in s['steps'], 'wm30:%d@moment:%d plans %s, lane R1 says step %d' % (n, bank, s and s['step'], k))
            if s:
                st = table[n]['steps'][k]
                free = k in reach_free(table[n]['steps'])  # re-derived here from the edges, independently of step_table.free_steps
                check(st['free'] == free, 'match %d step %d: free=%s but its entries say %s' % (n, k, st['free'], free))
                check(s['force'] == (not free) and s['entry_free'] == free, 'wm30:%d@moment:%d: force %s entry_free %s for a %s step' % (n, bank, s['force'], s['entry_free'], 'free' if free else 'guarded'))
                n_free += free
                n_guarded += not free
            n_plays += 1
        # a bank the chunk never plays
        other = next(x for x in (1001, 1002, 9999) if x not in plays)
        check(resolve.resolve('wm30:%d@moment:%d' % (n, other), ctx).status == 'error', 'wm30:%d@moment:%d (never played) was planned' % (n, other))
    # every real step of every match: free plans bare, guarded needs /force, a step the chunk lacks is refused
    n_steps = 0
    for n, m in sorted(table.items()):
        for k, st in m['steps'].items():
            n_steps += 1
            bare = resolve.resolve('wm30:%d@step:%d' % (n, k), ctx)
            forced = resolve.resolve('wm30:%d@step:%d/force' % (n, k), ctx)
            check((bare.status == 'plan') == (st['free'] and k != 0), 'wm30:%d@step:%d is %s but the step is %s' % (n, k, bare.status, 'free' if st['free'] else 'guarded'))
            check(forced.status == 'plan', 'wm30:%d@step:%d/force is %s: %s' % (n, k, forced.status, forced.reason))
        missing_step = next(x for x in range(0, 999) if x not in m['steps'])
        check(resolve.resolve('wm30:%d@step:%d/force' % (n, missing_step), ctx).status == 'error', 'wm30:%d@step:%d (not a step) was planned' % (n, missing_step))
    check(n_plays == 50 and n_free == 0 and n_guarded == 50, 'plays %d free %d guarded %d, expected 50 / 0 / 50 (review F2: no scene step is reachable with no test)' % (n_plays, n_free, n_guarded))
    s3 = step_of(resolve.resolve('wm30:3@moment:1090', ctx))
    check(s3 and s3['step'] == 2, 'Hogan v Andre bank 1090 is not step 2')
    # planted twins: each wrong table must be caught by the comparisons above
    import copy
    shifted = copy.deepcopy(table)
    shifted[3]['plays'] = {1090: 3}
    st3 = step_of(resolve.resolve('wm30:3@moment:1090', resolve.Context(step_table=shifted)))
    check(st3 is None or st3['step'] != truth[3][1090], 'planted wrong step index: the shifted table still gave R1\'s step')
    flipped = copy.deepcopy(table)
    flipped[0]['steps'][3]['free'] = True  # the m0 s3 shape (an unconditional setter inside a guarded step) claimed free: would be taken without force
    pf = resolve.resolve('wm30:0@step:3', resolve.Context(step_table=flipped))
    check(pf.status == 'plan' and flipped[0]['steps'][3]['free'] != (3 in reach_free(flipped[0]['steps'])), 'planted: a step wrongly marked free is not detectable by the re-derivation')
    check(resolve.resolve('wm30:0@step:3', ctx).status == 'error', 'the m0 s3 shape (unconditional setter after a guarded step) was planned without /force')
    # review F1 over the real table: the three flag-1 banks expect 25 -> 19, every other bank 25 -> 14
    for n, plays in sorted(truth.items()):
        for bank in plays:
            pl = resolve.resolve('wm30:%d@moment:%d' % (n, bank), ctx)
            ex = [x['any'] for x in pl.steps if x['op'] == 'expect_fire']
            want = '25 -> 19 (frame' if bank in (1010, 1330, 1400) else '25 -> 14 (frame'
            check(len(ex) >= 2 and want in ex[1], 'wm30:%d@moment:%d expects %s, got %s' % (n, bank, want, ex[1:2]))
    print('  real table: %d matches, %d plays (%d free, %d guarded) all resolve to lane R1\'s step; %d steps checked bare and /force; planted twins caught' %
          (len(truth), n_plays, n_free, n_guarded, n_steps))


def listing_main():
    """Pins of the numbers src/wm30_step.h hard-codes, against the generated listing of the game's functions (docs/systems/wm30.md)."""
    gen = os.path.join(os.environ.get('RDR_MAIN_CHECKOUT') or resolve.main_checkout(), 'generated', 'default')
    if not os.path.isdir(gen):
        raise RuntimeError('no generated code at %s' % gen)

    def func(name):
        for fn in sorted(os.listdir(gen)):
            if not fn.endswith('.cpp'):
                continue
            t = open(os.path.join(gen, fn), encoding='utf-8', errors='replace').read()
            i = t.find('DEFINE_REX_FUNC(%s)' % name)
            if i >= 0:
                j = t.find('\nDEFINE_REX_FUNC', i + 10)
                return t[i:j if j > 0 else len(t)]
        raise RuntimeError('%s not found under %s' % (name, gen))
    setter, generic_set, generic_get = func('sub_82731D80'), func('sub_82A9CAD8'), func('sub_82A9CB00')
    step = open(os.path.join(REPO, 'src', 'wm30_step.h'), encoding='utf-8').read()
    pins = [
        ('the variable is read at index -3 and the value at -2', 'ctx.r4.s64 = -3;' in setter and 'ctx.r4.s64 = -2;' in setter and setter.index('ctx.r4.s64 = -3;') < setter.index('ctx.r4.s64 = -2;')),
        ('variable 267 is compared', 'compare<int32_t>(ctx.r31.s32, 267, xer)' in setter and 'kStepVar = 267' in step),
        ('script index word 0x836EA814 (lis -2089877504 + -22508)', 'ctx.r11.s64 = -2089877504;' in setter and 'REX_LOAD_U32(ctx.r11.u32 + -22508)' in setter),
        ('script table 0x839C03E0 (lis -2086928384 + 992)', 'ctx.r10.s64 = -2086928384;' in setter and 'int64_t(992)' in setter),
        ('the value is stored at script object +3632', 'REX_STORE_U32(ctx.r11.u32 + 3632, ctx.r4.u32)' in setter and 'kObjStep = 3632' in step),
        ('a non-267 variable goes to the generic setter sub_82A9CAD8', 'sub_82A9CAD8(ctx, base)' in setter),
        ('the generic setter takes (var, value) in r3 / r4 and forwards to sub_82A9D8A8', 'ctx.r10.u64 = ctx.r3.u64;' in generic_set and 'sub_82A9D8A8(ctx, base)' in generic_set),
        ('the generic getter forwards r3 = var to sub_82A9D898', 'ctx.r4.u64 = ctx.r3.u64;' in generic_get and 'sub_82A9D898(ctx, base)' in generic_get),
    ]
    # review of lane 121: obj+3632 is a next-step slot committed by the object's tick methods (sub_8273A290 twice, sub_8273A418 once)
    for name, n_commits in (('sub_8273A290', 2), ('sub_8273A418', 1)):
        body = func(name)
        commit = ('ctx.r4.u64 = REX_LOAD_U32(ctx.r31.u32 + 3632);' in body and 'ctx.r3.s64 = 267;' in body and 'sub_82A9CAD8(ctx, base);' in body and
                  'REX_STORE_U32(ctx.r31.u32 + 3632, ctx.r11.u32);' in body and 'ctx.r11.s64 = -1;' in body)
        pins.append(('%s loads the slot (lwz r4,3632), calls sub_82A9CAD8(267, slot) and stores -1 back' % name, commit))
        pins.append(('%s commits %d time(s)' % (name, n_commits), body.count('sub_82A9CAD8(ctx, base);') == n_commits and body.count('ctx.r3.s64 = 267;') == n_commits))

    for what, ok in pins:
        check(ok, 'listing pin failed: %s' % what)
    print('  listing pins: %d checked against sub_82731D80 / sub_82A9CAD8 / sub_82A9CB00' % len(pins))


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else ''
    try:
        if mode == '--table':
            table_main()
        elif mode == '--listing':
            listing_main()
        else:
            check_address_and_resolver()
            check_game_side()
            check_wiring()
    except RuntimeError as x:
        check(False, 'cannot read an input: %s' % x)
    if problems:
        print('FAILED: %d problem(s)' % len(problems))
        return 1
    print('step selftest%s: all passed' % (' ' + mode if mode else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main())
