"""S101 lane 120 (+ lane 113's move line): the gate check `rumble_bus`. No game, no game data.

    py -I tools/bus/rumble_selftest.py

Compiles src/event_bus_rumble.h (the code src/event_bus.h's MatchTick calls: the Rumble eliminated / entered lines and the move line) against
synthetic guest words (tools/bus/rumble_test.cpp) and requires the exact event lines (tools/bus/fixtures/expect_<scenario>.txt, read and checked
by hand when written, plus the hand-written assertions below):
  rumble     type 23, G+80 = 38, G+81 = 2, counter 0 -> 1 -> 3 (a same-frame double: the LAST slot's character has the LOWER index), [0x835E1928]
             0 -> 1, a clock that flaps with no seq change, starters = the slots 0..5 whose character is active
  twinorder  the same with the last slot's character at the HIGHER index
  resync     a counter jump over 40, a counter that drops (a new match), the entered counter jumping, a spawn slot word of -1 (signed)
  type0      the same words with type 0: nothing
  cap        an entrant whose character is never built: entered_id with id -1 after 600 frames
  move       a finisher on Brock's character, a repeat with no clear in between (the documented LIMIT: one line), a recycled character holding a
             STALE finisher (no line on first sight), then its own finisher (a line)
PLANTED TWINS (a text replacement in a copy of the header, must hit exactly once, must change the output) are listed in `twins` below. Two
TEXT CHECKS (not twins) pin that src/event_bus.h calls this header and keeps its filters. The syntax-only compile of event_bus.h
(tools/bus/syntax_check.py) runs here too, with a planted syntax error that it must catch.
Exit 0 = all passed.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import types

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
HDR = os.path.join(REPO, 'src', 'event_bus_rumble.h')
SCENARIOS = ['rumble', 'twinorder', 'resync', 'type0', 'cap', 'move', 'moveall']
problems = []


def check(ok, msg):
    if not ok:
        problems.append(msg)
        print('FAIL: ' + msg)


def clang():
    for c in (shutil.which('clang++'), r'C:\Program Files\LLVM\bin\clang++.exe', '/usr/bin/clang++'):
        if c and os.path.exists(c):
            return c
    raise RuntimeError('no clang++ found (tools\\env.ps1 puts the project\'s LLVM on PATH)')


def build(tmp, header, name):
    exe = os.path.join(tmp, name + '.exe')
    r = subprocess.run([clang(), '-std=c++17', '-O0', '-I', os.path.join(REPO, 'src'), '-DHDR="%s"' % header.replace('\\', '/'),
                        os.path.join(HERE, 'rumble_test.cpp'), '-o', exe], capture_output=True, text=True, timeout=240)
    if r.returncode:
        raise RuntimeError('compile failed (%s): %s' % (name, r.stderr[-1500:]))
    return exe


def run(exe, scenario):
    r = subprocess.run([exe, scenario], capture_output=True, text=True, timeout=60)
    if r.returncode:
        raise RuntimeError('%s %s exited %d' % (exe, scenario, r.returncode))
    return r.stdout.replace('\r\n', '\n').splitlines()


def expected(name):
    p = os.path.join(HERE, 'fixtures', 'expect_%s.txt' % name)
    with open(p, encoding='utf-8') as f:   # raises when missing: a checker that cannot read its input fails
        return f.read().replace('\r\n', '\n').splitlines()


def outputs(exe):
    return {k: run(exe, k) for k in SCENARIOS}


def mutated(tmp, pairs, name):
    src = open(HDR, encoding='utf-8').read().replace('\r\n', '\n')
    for old, new in pairs:
        assert src.count(old) == 1, 'planted fault text not found exactly once: %r (%d)' % (old[:70], src.count(old))
        src = src.replace(old, new)
    p = os.path.join(tmp, name + '.h')
    open(p, 'w', encoding='utf-8').write(src)
    return p


def main():
    tmp = tempfile.mkdtemp(prefix='l120_rumble_')
    exe = build(tmp, HDR, 'real')
    got = outputs(exe)
    for k in SCENARIOS:
        want = expected(k)
        check(got[k] == want, 'scenario %s: the bus lines differ\n  want %s\n  got  %s' % (k, '\n       '.join(want) or '(none)', '\n       '.join(got[k]) or '(none)'))
    # hand-written pins (the review's probe: a same-frame group pairs each victim with its own id / the last slot's character)
    r = got['rumble']
    check(any('"n": 3, "slot": 4, "id": 88' in x and '"victim_idx": 6' in x for x in r) and any('"n": 2, "slot": -1, "id": 99' in x and '"victim_idx": 7' in x for x in r),
          'a same-frame group: the last slot\'s character (participant 6, id 88) is n=3, the other victim (7, id 99) is n=2')
    t = got['twinorder']
    check(any('"n": 3, "slot": 4, "id": 99' in x and '"victim_idx": 7' in x for x in t) and any('"n": 2, "slot": -1, "id": 88' in x and '"victim_idx": 6' in x for x in t),
          'the same group with the last slot at the higher index pairs the other way')
    check(sum('"eliminated"' in x for x in r) == 3 and not any('"starter"' in x for x in r if '"f": 107' in x), 'a same-frame double emits 2 events (+1); the flapping clock re-emits nothing')
    check(sum('"starter"' in x for x in r) == 2 and all('"slot": %d' % i in x for i, x in enumerate([y for y in r if '"starter"' in y])), 'two starters, from the active characters of slots 0 and 1')
    check(got['type0'] == [], 'type 0 emits nothing from this branch')
    check(len([x for x in got['move'] if '"finisher"' in x]) == 2 and not any('"f": 13' in x or '"f": 14' in x for x in got['move']),
          'move: the no-clear repeat (f13) and the stale recycled finisher (f14) emit nothing; its own finisher (f15) does')
    check(any('"f": 302' in x and '"slot": -1' in x for x in got['resync']), 'a spawn slot word 0xFFFFFFFF is read as -1')

    twins = [
        ('the first sight of a new char pointer does not reseed the move key', [("      st.ptr[slot] = c;\n      st.seen[slot] = key;\n      continue;", "      st.ptr[slot] = c;\n      continue;")]),
        ('a stale finisher is emitted on first sight', [("      st.seen[slot] = key;\n      continue;\n    }\n    if (cleared) {", "      st.seen[slot] = -2;\n    }\n    if (cleared) {")]),
        ('remaining off by one', [('st.entrants - k : -1', 'st.entrants - k + 1 : -1')]),
        ('only the last of a same-frame group emitted', [('for (int64_t k = p + 1; k <= n; ++k) {', 'for (int64_t k = n; k <= n; ++k) {')]),
        ('victims paired by position (the review\'s wrong pairing)', [("      if (last) v = last_v;\n      else if (count == 2 && rest.size() == 1) v = rest[0];\n",
                                                                     "      v = size_t(k - p - 1) < victims.size() ? victims[size_t(k - p - 1)] : -1;\n")]),
        ('the Rumble type guard gone', [('if (!IsRumbleType(type)) {', 'if (false) {')]),
        ('the entered id is never emitted', [('if (last && slot >= 0 && slot < 9) st.pending.push_back', 'if (false) st.pending.push_back')]),
        ('the entered_id cap gone', [('frame >= pd.since + kEnteredIdCapFrames', 'false')]),
        ('a counter jump over 40 is not a resync', [('n > st.elim && n - st.elim > kMaxStep', 'false')]),
        ('a dropped counter does not start a new match', [('  if (st.started && int64_t(m.r32(kElimCount)) < st.elim) st.Reset();  // the counter dropped: a new match\n', '')]),
        ('the clock flapping resets the Rumble and the starters come again', [
            ("  if (!st.started) {\n    if (!clock_running) return true;", "  if (st.started && !clock_running) st.Reset();\n  if (!st.started) {\n    if (!clock_running) return true;"),
            ("if (st.elim <= int64_t(m.r8(kG + 82)) && int64_t(m.r32(kEnteredCount)) == 0) {", "if (true) {")]),
        ('the slot words read unsigned', [('const int64_t slot = int32_t(m.r32(kSpawnSlot));', 'const int64_t slot = int64_t(m.r32(kSpawnSlot));')]),
    ]
    for i, (name, pairs) in enumerate(twins):
        try:
            h = mutated(tmp, pairs, 'twin%d' % i)
            tx = build(tmp, h, 'twin%d' % i)
            tg = outputs(tx)
        except AssertionError as x:
            check(False, 'twin "%s" could not be planted: %s' % (name, x))
            continue
        except RuntimeError as x:
            check(False, 'twin "%s" did not compile: %s' % (name, x))
            continue
        check(tg != got, 'PLANTED TWIN "%s" was not caught (the scenarios print the same lines)' % name)

    # TEXT CHECKS (not twins): the shipped bus calls this code, not a copy, and keeps its filters
    bus = open(os.path.join(REPO, 'src', 'event_bus.h'), encoding='utf-8').read().replace('\r\n', '\n')
    i = bus.index('inline void MatchTick')
    mt = bus[i:bus.index('// The owner update')]
    check('#include "event_bus_rumble.h"' in bus and 'rumble::Tick(g_rumble, mem, frame,' in mt and 'rumble::MoveTick(g_moves' in mt,
          'TEXT CHECK: src/event_bus.h MatchTick calls rumble::Tick and rumble::MoveTick from the shipped header')
    check('&& !is_rumble) {' in mt and 'if (id == 255) continue;' in mt and 'clock_on' not in bus,
          'TEXT CHECK: the per-slot path skips Rumbles and id 255, with no clock gate (elimination tag / chamber still emit)')
    check('g_rumble.Reset();' in bus and 'R16(' in bus and 'wwe2k14_events_moves' in bus, 'TEXT CHECK: the match leave resets the state; R16 and the cvar exist')

    # the syntax-only compile of the shipped event_bus.h, and its planted syntax error
    sc = os.path.join(HERE, 'syntax_check.py')
    r = subprocess.run([sys.executable, '-I', sc], capture_output=True, text=True, timeout=400)
    check(r.returncode == 0, 'event_bus.h syntax check: %s' % (r.stdout + r.stderr)[-800:])
    bad = os.path.join(tmp, 'bad')
    os.makedirs(os.path.join(bad, 'src'))
    shutil.copy(HDR, os.path.join(bad, 'src', 'event_bus_rumble.h'))
    open(os.path.join(bad, 'src', 'event_bus.h'), 'w', encoding='utf-8').write(bus.replace('inline Last g_last;', 'inline Last g_last PLANTED_SYNTAX_ERROR;', 1))
    r = subprocess.run([sys.executable, '-I', sc, '--root', bad], capture_output=True, text=True, timeout=400)
    check(r.returncode != 0 and 'does not compile' in r.stdout, 'PLANTED TWIN "a syntax error in event_bus.h" was not caught by the syntax check')

    # the kept run 6054 shape (3 id-255 lines 460 frames before the bell) through the grammar, and its twin without the filters
    sys.path.insert(0, os.path.join(REPO, 'tools', 'goto'))
    import when as W
    k = W.load_events(os.path.join(HERE, 'fixtures', 'rumble_events_6054.jsonl'))
    check(len(k) == 4 and sum(e.get('id') == 255 for e in k) == 3, 'the fixture holds the 3 false lines and the bell')
    try:
        W.evaluate(k, '@eliminated')
        check(False, '@eliminated resolved on a stream that holds only the start burst')
    except W.WhenError:
        pass
    src = open(os.path.join(REPO, 'tools', 'goto', 'when.py'), encoding='utf-8').read().replace('\r\n', '\n')
    mut = src.replace("and e.get('id') != 255   # S101 lane 120: id 255", "   # S101 lane 120: id 255").replace("if c.name == 'eliminated' and i <= first_bell:", 'if False:')
    assert mut != src and mut.count('if False:') == 1
    mm = types.ModuleType('when_mutant')
    mm.__file__ = os.path.join(REPO, 'tools', 'goto', 'when.py')
    exec(compile(mut, 'when_mutant', 'exec'), mm.__dict__)
    try:
        check(mm.evaluate(k, '@eliminated').frame == 4931, 'twin without the filters resolves to the false frame 4931')
    except mm.WhenError:
        check(False, 'PLANTED TWIN "the start burst counted" was not caught: the false lines did not return')

    shutil.rmtree(tmp, ignore_errors=True)
    print('rumble_bus selftest: %s' % ('%d problems' % len(problems) if problems else '0 problems (all checks passed)'))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
