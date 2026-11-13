"""S101 lane 108 (L-5): offline checks of `wm30:N@fire:BANK` / `!wm30 fire BANK`. No game runs.

    py -I tools/goto/fire_selftest.py              the address, the resolver's table guard (synthetic table), the game-side parse rule and the
                                                   guard truth table (src/wm30_fire_args.h COMPILED with clang++ and driven), wiring pins, planted twins
    py -I tools/goto/fire_selftest.py --table      the same resolver guard over lane R1's REAL scene table (reads the player's pacs)
    py -I tools/goto/fire_selftest.py --listing    pins of every address / offset src/wm30_fire.h uses against the generated listing of the game's
                                                   Lua native 1003 (sub_82736790) and its preload writer (reads generated/, never printed)

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
    raise RuntimeError('no clang++ found (tools\\env.ps1 puts the project\'s LLVM on PATH): the fire parse / truth table test cannot run')


def build_test(header_text, tmp, tag):
    """Compiles tools/goto/fire_test.cpp against `header_text` as src/wm30_fire_args.h (a copy under tmp, so a planted twin never touches the tree)."""
    inc = os.path.join(tmp, tag)
    os.makedirs(inc, exist_ok=True)
    with open(os.path.join(inc, 'wm30_fire_args.h'), 'w', encoding='utf-8') as f:
        f.write(header_text)
    exe = os.path.join(inc, 'fire_test.exe')
    r = subprocess.run([find_clang(), '-std=c++17', '-I', inc, os.path.join(HERE, 'fire_test.cpp'), '-o', exe], capture_output=True, text=True, timeout=180)
    if r.returncode != 0 or not os.path.isfile(exe):
        raise RuntimeError('fire_test did not compile: %s' % (r.stdout + r.stderr)[-600:])
    return exe


def run_lines(exe, lines):
    r = subprocess.run([exe], input='\n'.join(lines) + '\n', capture_output=True, text=True, timeout=60)
    out = r.stdout.split('\n')
    if out and out[-1] == '':
        out.pop()
    if len(out) != len(lines):
        raise RuntimeError('fire_test answered %d lines for %d inputs' % (len(out), len(lines)))
    return out


# ---- the truth table, written out independently of the header (the spec): returns (class, key words) ----
def expected(started, in_game, obj, g472, g468, flag, held=0):
    if not started:
        return ('refused', 'not in a 30 Years match')
    if not in_game:
        return ('busy', 'still loading')
    if g472:
        return ('refused', 'already ended')
    if g468:
        return ('refused', 'a scene is already running')
    if flag != 0:
        return ('refused', 'would remove the kind-6 tail')
    if held >= 10:  # lane 144: the match clock is held (J3's paused match)
        return ('busy', 'the match clock is held at 15')
    if not obj:
        return ('busy', 'script object does not exist')
    return None


def truth_cases():
    cases = []
    for st, ig, ob, f472, f468, flag, held in itertools.product((0, 1), (0, 1), (0, 1), (0, 7), (0, 1, 2), (0, 1), (0, 9, 10, 50)):  # lane 144: clock_held
        cases.append(('verdict %d %d %d %d %d %d 15 %d' % (st, ig, ob, f472, f468, flag, held), expected(st, ig, ob, f472, f468, flag, held)))
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


PARSE_GOOD = [('fire 1090', (1090, 0, 0, 0)), ('fire 1090 force', (1090, 1, 0, 0)), ('  fire   65535  ', (65535, 0, 0, 0)), ('fire 1', (1, 0, 0, 0)), ('fire 2110\tforce', (2110, 1, 0, 0)),
              ('fire 1780 actor=1 mode=1', (1780, 0, 1, 1)), ('fire 1710 mode=-1', (1710, 0, 0, -1)), ('fire 1400 mode=1 actor=1 force', (1400, 1, 1, 1)), ('fire 5 mode=9', (5, 0, 0, 9))]
PARSE_BAD = ['fire actor=1', 'fire 5 actor=2', 'fire 5 actor=', 'fire 5 actor=1 actor=0', 'fire 5 mode=10', 'fire 5 mode=-2', 'fire 5 mode=', 'fire 5 mode=-', 'fire 5 mode=1 mode=1', 'fire 5 force force', 'fire 5 actor=1 mode=1 force x', 'fire 5 Force', 'fire 5 actor', 'fire', 'fire 0', 'fire 65536', 'fire -1', 'fire +5', 'fire 10x', 'fire 1090 forced', 'fire 1090 force x', 'fire 1e3', '1090', 'fire abc', 'fire 123456',
             'fire force', 'fire 1090 1', 'fires 1090', 'off', '', '3']


def parse_cases():
    # the strings the resolver's executor builds, over every bank it can be asked for in the real range plus force
    good = [(resolve.fire_arg(b, f, ac, mo), (b, int(f), ac, mo)) for b in (1, 1010, 1090, 2110, 65535) for f in (False, True) for ac in (0, 1) for mo in (-1, 0, 1)]
    good += PARSE_GOOD
    return [('parse ' + t, ('FIRE',) + tuple(v)) for t, v in good] + [('parse ' + t, ('ERR',)) for t in PARSE_BAD]


def check_parse(exe, cases):
    out = run_lines(exe, [c[0] for c in cases])
    bad = []
    for (text, want), got in zip(cases, out):
        g = got.split()
        have = ('FIRE',) + tuple(int(x) for x in g[1:5]) if g[0] == 'FIRE' else (g[0],)
        if have != want:
            bad.append('%r: want %s, got %s' % (text, want, got[:60]))
    return bad


ROUTE = [('fire 5', 1), ('  fire', 1), ('5', 0), ('off', 0), ('', 0), ('firex 5', 0), ('5 10101', 0)]


def check_route(exe):
    out = run_lines(exe, ['route ' + t for t, _ in ROUTE])
    return ['route %r: want %d, got %s' % (t, w, o) for (t, w), o in zip(ROUTE, out) if o != 'ROUTE %d' % w]


# ---- the address and the resolver's table guard (synthetic table + synthetic SCEX list: no game files) ----
def synthetic_ctx(rows):
    scex = {'eras': {0: [10300, 10301, 10302], 1: [], 2: [], 3: [], 4: []},
            'matches': {3: {'era': 0, 'code': 10300, 'record': 3}, 7: {'era': 0, 'code': 10300, 'record': 7}}}
    ctx = resolve.Context(scex=scex, scene_rows=rows)
    ctx._scene_args = {3: {1090: dict(actor=0, mode=0, flag=0), 1100: dict(actor=1, mode=1, flag=1)}, 7: {}}
    return ctx


def check_address_and_guard():
    for t in ('wm30:3@fire:1090', 'wm30:3@fire:1090/force', 'wm30:45@fire:65535'):
        a = address.parse(t)
        check(str(a) == t and a.fire > 0, 'address %s did not round-trip (%s)' % (t, a))
    for t in ('wm30:3@fire:0', 'wm30:3@fire:', 'wm30:3@fire:abc', 'wm30:3@fire:70000', 'wm30:3@fire:1090/forced', 'wm30:3@moment:1@fire:5', 'wm30:46@fire:1090',
              'wm30:3@fire:1090@fire:1090', 'match:Cena vs Orton@fire:5'):
        try:
            address.parse(t)
            check(False, 'address %s should be refused' % t)
        except address.AddressError:
            pass
    ctx = synthetic_ctx({3: [1090], 7: []})
    p = resolve.resolve('wm30:3@fire:1090', ctx)
    check(p.status == 'plan', 'wm30:3@fire:1090 is %s (%s)' % (p.status, p.reason))
    ops = [s['op'] for s in p.steps]
    check(ops == ['nav', 'wm30_arm', 'expect_log', 'events_mark', 'wm30_to_bell', 'wait_event', 'wm30_clear_box', 'wm30_fire', 'expect_fire', 'expect_fire', 'expect_fire', 'expect_fire'], 'plan steps are %s' % ops)
    exp = [s for s in p.steps if s['op'] == 'expect_fire']
    check('25 -> 14 (frame' in exp[1]['any'] and '25 -> 19 (frame' in exp[1]['fail'] and '14 -> 10 (frame' in exp[2]['any'] and '14 -> 8 (frame' in exp[2]['any'] and '14 -> 21 (frame' in exp[2]['fail']
          and '14 -> 10 (frame' not in exp[2]['fail'] and 'back in the match (state 12)' in exp[3]['any'][0] and '14 -> 21 (frame' in exp[3]['fail']
          and any('CPU' in x and 'finish the match' in x for x in p.notes),  # lane 150
          'the expect_fire steps lost the 25 -> 14 / 14 -> 10 / back-in-the-match states or the route note')
    check(any(s['op'] == 'wm30_fire' and s['bank'] == 1090 and not s['force'] for s in p.steps), 'the fire step lost its bank')
    check(resolve.resolve('wm30:3@fire:1100', ctx).status == 'error', 'a bank the chunk never plays was planned')
    check(resolve.resolve('wm30:7@fire:1090', ctx).status == 'error', 'a match with no scene accepted a bank')
    pf = resolve.resolve('wm30:3@fire:1100/force', ctx)
    check(pf.status == 'plan' and any(s['op'] == 'wm30_fire' and s['force'] and s['actor'] == 0 for s in pf.steps), '/force did not skip the table guard')
    ctx2 = synthetic_ctx({3: [1090, 1100], 7: []})
    p2 = resolve.resolve('wm30:3@fire:1100', ctx2)
    st = [s for s in p2.steps if s['op'] == 'wm30_fire'][0]
    check(st['actor'] == 1 and st['mode'] == 1, 'the resolver did not fill actor / mode from the table: %s' % st)
    check(any('flag-1' in n for n in p2.notes), 'a flag-1 bank has no note that it is fired with flag 0')
    check(resolve.resolve('wm30:3', ctx).steps == resolve.resolve('wm30:3', ctx).steps and all(s['op'] != 'wm30_fire' for s in resolve.resolve('wm30:3', ctx).steps),
          'a plain wm30:N gained a fire step')
    # planted: a guard that is not applied must be seen by the same comparison
    bad = resolve.fire_problem(synthetic_ctx({3: []}), address.parse('wm30:3@fire:1090'))
    check(bool(bad), 'planted: an empty table did not refuse bank 1090')


def check_game_side():
    header = open(os.path.join(REPO, 'src', 'wm30_fire_args.h'), encoding='utf-8').read()
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
            'G+468 guard dropped': ('if (in.g468 != 0)', 'if (false && in.g468 != 0)'),
            'flag guard dropped': ('if (in.flag != 0)', 'if (false && in.flag != 0)'),
            'not-started guard dropped': ('if (!in.started)', 'if (false && !in.started)'),
            'match-ended guard dropped': ('if (in.g472 != 0)', 'if (false && in.g472 != 0)'),
            'object guard dropped': ('if (!in.have_object)', 'if (false && !in.have_object)'),
            'loading guard dropped': ('if (!in.in_game)', 'if (false && !in.in_game)'),
            'clock-held guard dropped (lane 144)': ('if (in.clock_held >= kClockHeldFrames)', 'if (false && in.clock_held >= kClockHeldFrames)'),
            'clock-held threshold off by one (lane 144)': ('in.clock_held >= kClockHeldFrames', 'in.clock_held > kClockHeldFrames'),
            'duplicate force allowed in parse': ('if (out.force) return', 'if (false) return'),
            'actor ceiling removed': ('std::stoi(v) > kMaxActor', 'false'),
            'mode floor removed': ('m < kMinMode ||', 'false ||'),
        }
        for what, (old, new) in twins.items():
            if old not in header:
                check(False, 'planted twin "%s": anchor %r is gone from the header (the test lost its anchor)' % (what, old))
                continue
            tb = os.path.join(tmp, 'twin_' + re.sub(r'\W', '', what))
            exe2 = build_test(header.replace(old, new, 1), tmp, 'twin_' + re.sub(r'\W', '', what))
            caught = bool(check_truth(exe2, tc)) or bool(check_parse(exe2, pc))
            check(caught, 'planted twin (%s) was not caught by the truth table / parse cases' % what)
    print('  game side: parse %d cases, routing %d, truth table %d combinations; %d planted twins caught' % (len(pc), len(ROUTE), len(tc), len(twins)))


def between(text, a, b):
    i = text.index(a)
    return text[i:text.index(b, i)]


def check_wiring():
    fire = open(os.path.join(REPO, 'src', 'wm30_fire.h'), encoding='utf-8').read()
    jump = open(os.path.join(REPO, 'src', 'wm30_jump.h'), encoding='utf-8').read()
    sd = open(os.path.join(REPO, 'src', 'scene_director.h'), encoding='utf-8').read()
    fp = open(os.path.join(REPO, 'src', 'flow_probe.h'), encoding='utf-8').read()
    app = open(os.path.join(REPO, 'src', 'wwe2k14_app.h'), encoding='utf-8').read()
    args = open(os.path.join(REPO, 'src', 'wm30_fire_args.h'), encoding='utf-8').read()
    check('constexpr int kFlag = 0;' in args, 'kFlag is no longer 0 (flag 0 is what keeps the kind-6 tail and the resume)')
    check('call(sub_82736790, {kFakeL})' in fire and 'REX_EXTERN(sub_82736790)' in fire, 'the fire does not CALL the game\'s sub_82736790')
    fn = between(fire, 'inline std::string Fire(', '// The game thread')
    check(fn.index('FireVerdict(in)') < fn.index('call(sub_82736790'), 'the guards must run before the game function is called')
    check(fn.index('FireVerdict(in)') < fn.index('ScriptObject()'), 'review F2: the script object must be read only after the earlier guards')
    check('preloaded' not in between(args, 'inline std::string FireVerdict', '}  // namespace'), 'review F1: a preloaded refusal is back in the verdict')
    check(re.search(r'kFakeL = 0x0000[0-9A-F]{4};', fire) is not None, 'the sentinel L must be below 0x10000')
    check('g_post_logged' in fire and 'kWatchFrames = 20000' in fire, 'review F4: the watch must run until the state after 14 is logged')
    check('index == -3' in fire and 'index == -2' in fire, 'the actor / mode argument reads are gone from the hook')
    hook = fire[fire.index('REX_HOOK_RAW(sub_82C4A538)'):]
    check('ctx.r3.u32 == kFakeL && g_fake_active' in hook and '__imp__sub_82C4A538(ctx, base)' in hook and 'index == -6' in hook,
          'the Lua argument hook lost its sentinel test, its bank slot (-6) or the pass-through')
    live = between(fire, 'inline std::string LiveCommand', 'inline const bool g_registered')
    check('U32(' not in live and 'Guest(' not in live, 'the fire LiveCommand reads guest memory on the pipe thread')
    check(jump.index('IsFireCommand(args)') < jump.index('ParseArmArgs(args, a)'), 'wm30_jump.h parses the match number before routing fire')
    check('g_fire_tick(ctx, base)' in sd, 'scene_director Tick does not call the fire tick')
    check(fp.count('wm30_fire::g_watch') >= 2 and 'LogFlowSet' in fp and 'LogFlowNext' in fp, 'flow_probe.h lost the fire watch calls')
    check('#include "wm30_fire.h"' in app, 'wwe2k14_app.h does not include wm30_fire.h')
    check('in.clock_held = g_clk_held' in fn and 'ClockTick(seq);' in fire and 'g_clk_held = (c > 0' in fire, 'lane 144: the fire verdict lost its clock-held input / the tracker')
    check('seq={} clock {}' in fire and 'last flow state entered {}) clock {}' in fire, 'lane 144: the fire watch lines lost their clock field')
    check('seq == kSeqMainMenu' in fire and 'g_started.store(false)' in fire, 'review F2: entering sequence 75 no longer clears the started flag')
    check('g_fire_started.store(true)' in jump, 'the era-load hook no longer records a 30 Years start')
    fw = open(os.path.join(REPO, 'src', 'wm30_fire_watch.h'), encoding='utf-8').read()  # lane 150
    check('id == 10 ? "RESUMED (10 -> 12, the running match)"' in fw and 'id == 8 ? "RESUMED through 9 -> 10"' in fw and 'id == 21 ? "MATCH-END path (21 -> 24 -> 4)"' in fw
          and 'id == 15 ? "the G+18576 == 9 branch [U]"' in fw and 'OTHER PATH' not in fw, 'lane 150: the post-14 labels are not 10 / 8 RESUMED, 21 MATCH-END, 15 [U]')
    check('fire: back in the match (state 12) at frame' in fw and 'over 60 frames (' in fw and '"running" : "held"' in fw, 'lane 150: the back-in-the-match line text changed')
    check('g_back_frame' in fire and 'BackLine(' in fire and 'WatchDone()' in fire and 'ResetBack();' in fire, 'lane 150: wm30_fire.h Tick lost the back-in-the-match watch')
    check('ResetBack();' in open(os.path.join(REPO, 'src', 'wm30_step.h'), encoding='utf-8').read(), 'lane 150: wm30_step.h does not reset the back-in-the-match state')
    print('  wiring pins ok')


def table_main():
    ctx = resolve.Context()
    rows = ctx.scene_rows
    check(len(rows) == 46, 'the real scene table has %d matches, not 46' % len(rows))
    n_plans = n_err = 0
    for n, banks in sorted(rows.items()):
        for b in banks:
            p = resolve.resolve('wm30:%d@fire:%d' % (n, b), ctx)
            check(p.status == 'plan', 'wm30:%d@fire:%d (a bank the chunk plays) is %s: %s' % (n, b, p.status, p.reason))
            n_plans += p.status == 'plan'
        other = next(x for x in (1, 2, 3, 99, 65535) if x not in banks)
        p = resolve.resolve('wm30:%d@fire:%d' % (n, other), ctx)
        check(p.status == 'error', 'wm30:%d@fire:%d (a bank the chunk never plays) is %s' % (n, other, p.status))
        n_err += p.status == 'error'
    check(resolve.resolve('wm30:3@fire:1090', ctx).status == 'plan', 'Hogan v Andre bank 1090 is not a plan')
    check(rows.get(3) == [1090], 'match 3 plays %s, lane R1 says [1090]' % rows.get(3))
    args = {b: (v['actor'], v['mode'], v['flag']) for m in ctx.scene_args.values() for b, v in m.items()}
    check({b for b, v in args.items() if v[0] == 1} == {1150, 1170, 1190, 1200, 1340, 1390, 1400, 1750, 1780, 2000, 2010}, 'the actor-1 banks are %s' % sorted(b for b, v in args.items() if v[0] == 1))
    check({b: v[1] for b, v in args.items() if v[1]} == {1410: 1, 1780: 1, 1710: -1}, 'the non-zero modes are %s' % {b: v[1] for b, v in args.items() if v[1]})
    check({b for b, v in args.items() if v[2] == 1} == {1010, 1330, 1400}, 'the flag-1 banks are %s' % sorted(b for b, v in args.items() if v[2] == 1))
    p14 = resolve.resolve('wm30:%d@fire:1780' % [n for n, b in rows.items() if 1780 in b][0], ctx)
    st = [s for s in p14.steps if s['op'] == 'wm30_fire'][0]
    check(st['actor'] == 1 and st['mode'] == 1, 'bank 1780 plans actor %s mode %s, the table says 1 / 1' % (st['actor'], st['mode']))
    # planted: the same table without 1090 must refuse it
    twin = resolve.Context(scex=ctx.scex, scene_rows={k: [b for b in v if b != 1090] for k, v in rows.items()})
    check(resolve.resolve('wm30:3@fire:1090', twin).status == 'error', 'planted: a table without 1090 still planned wm30:3@fire:1090')
    print('  real table: %d matches, %d played banks all plan, %d unplayed-bank probes all refused; planted table twin caught' % (len(rows), n_plans, n_err))


def listing_main():
    """Pins of the numbers src/wm30_fire.h hard-codes, against the generated listing of the game's functions (docs/systems/wm30.md)."""
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
    f1003 = func('sub_82736790')
    fire = open(os.path.join(REPO, 'src', 'wm30_fire.h'), encoding='utf-8').read()
    pins = [
        ('the five Lua argument reads -6..-2', all(('ctx.r4.s64 = %d;' % k) in f1003 for k in (-6, -5, -4, -3, -2))),
        ('script index word 0x836EA814 (lis -2089877504 + -22508)', 'ctx.r11.s64 = -2089877504;' in f1003 and 'REX_LOAD_U32(ctx.r11.u32 + -22508)' in f1003 and 'kScriptIndexWord = 0x836EA814' in fire),
        ('script table 0x839C03E0 (lis -2086928384 + 992)', 'ctx.r10.s64 = -2086928384;' in f1003 and 'int64_t(992)' in f1003 and 'kScriptTable = 0x839C03E0' in fire),
        ('index range 0..7', 'compare<int32_t>(ctx.r11.s32, 8, xer)' in f1003 and 'idx >= 8' in fire),
        ('obj+3592 == 1 test', 'REX_LOAD_U32(ctx.r23.u32 + 3592)' in f1003 and 'kObjFlushed = 3592' in fire),
        ('obj+3596 six words', 'int64_t(3596)' in f1003 and 'compare<int32_t>(ctx.r11.s32, 6, xer)' in f1003 and 'kObjPreload = 3596' in fire and 'k < 6' in fire),
        ('request table T = B + 1140', 'int64_t(1140)' in f1003),
        ('flag 0 adds the kind-6 record (kind 6)', 'ctx.r4.s64 = 6;' in f1003 and 'sub_827F5D90' in f1003),
        ('G+468 is set by the function (listing offset 468 appears)', '468' in f1003 or 'sub_823AE0E8' in f1003),
    ]
    for what, ok in pins:
        check(ok, 'listing pin failed: %s' % what)
    print('  listing pins: %d checked against sub_82736790' % len(pins))


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else ''
    try:
        if mode == '--table':
            table_main()
        elif mode == '--listing':
            listing_main()
        else:
            check_address_and_guard()
            check_game_side()
            check_wiring()
    except RuntimeError as x:
        check(False, 'cannot read an input: %s' % x)
    if problems:
        print('FAILED: %d problem(s)' % len(problems))
        return 1
    print('fire selftest%s: all passed' % (' ' + mode if mode else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main())
