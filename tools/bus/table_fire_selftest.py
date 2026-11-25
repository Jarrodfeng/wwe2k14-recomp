"""S110 FIRETABLE: the check of src/table_fire_core.h (the fire wish's one pressed call of the game's table-fire step sub_825FB230, and the
replay skip's one call of the game's skip sub_826071F8). No game.

    py -I tools/bus/table_fire_selftest.py

Compiles tools/bus/table_fire_test.cpp against src/ (clang++ -std=c++17) and runs it; then each PLANTED TWIN (a copy of the core with one
fault, in a temp folder) must make it fail. TEXT CHECKS pin the wiring: one hook each of sub_825FB230 and sub_82607778 in src/*.h, in
src/table_fire.h; each runs the original and nothing else with no wish; the skip calls the HOOKED sub_826071F8 (src/replay_fx_skip.h keeps
that hook); situation_director.h includes table_fire.h (its one includer) and clears the wishes outside a match; the core routes both verbs.
Exit 0 = all passed.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
problems = []

# (name, pattern, replacement): each must match exactly once in src/table_fire_core.h and make the unit FAIL
PLANTS = [
    ('press_not_restored', r'  wr\(chooser \+ kCtxPress, press\);\n', ''),
    ('press_not_set', r'wr\(chooser \+ kCtxPress, 1u\);', 'wr(chooser + kCtxPress, 0u);'),
    ('queued_ignores_before', r'return before == kNoLink && after != kNoLink;', 'return after != kNoLink;'),
    ('press_offset_52', r'kCtxPress = 48;', 'kCtxPress = 52;'),
    ('link_offset_1336', r'kLinkOff = 1340;', 'kLinkOff = 1336;'),
    ('skip_in_any_state', r'return g_skip\.load\(std::memory_order_acquire\) && state == kPlaying;', 'return g_skip.load(std::memory_order_acquire);'),
    ('take_skip_any_state', r'  if \(state_after != kSkipFade\) return false;\n', ''),
    ('queued_keeps_armed', r'g_fire\[seat\]\.store\(2, std::memory_order_release\);', 'g_fire[seat].store(1, std::memory_order_release);'),
    ('recount_counts_queued', r'== 1 \? 1 : 0;', '!= 0 ? 1 : 0;'),
    ('skip_without_call', r'  skip\(\);\n', ''),
    ('clear_keeps_skip', r'  g_skip\.store\(false, std::memory_order_release\);\n  g_touched', '  g_touched'),
    ('state_seat_nine', r"c < char\('0' \+ kSeats\)", "c <= char('0' + kSeats)"),
    # S110 FIRETABLE review: the chooser's own press rule (sub_826056E0 +A0..+EC)
    ('press_rule_ignores_follow_up', r'return !act_clears && !special_clears && !has_follow_up;', 'return !act_clears && !special_clears;', 'chooser_press_core.h'),
    ('press_rule_ignores_act_set', r'return !act_clears && !special_clears && ', 'return !special_clears && ', 'chooser_press_core.h'),
    # S110 FIRETABLE fix 2 (run 318): the unserved skip is reported at the replay's end; the game's replay lock refuses the verb
    ('expire_keeps_the_wish', r'return g_skip\.exchange\(false, std::memory_order_acq_rel\);', 'return g_skip.load(std::memory_order_acquire);'),
    ('lock_ignored', r'inline bool ReplayLocked\(uint32_t word\) \{ return word != 0; \}', 'inline bool ReplayLocked(uint32_t word) { return false; }'),
]
TEXT = [
    ('src/table_fire.h', 'REX_HOOK_RAW(sub_825FB230)', 'the table-fire step is hooked in table_fire.h'),
    ('src/table_fire.h', 'REX_HOOK_RAW(sub_82607778)', 'the replay play handler is hooked in table_fire.h'),
    ('src/table_fire.h', 'if (g_armed.load(std::memory_order_acquire) == 0) {\n    __imp__sub_825FB230(ctx, base);\n    return;\n  }',
     'no fire wish: the original and nothing else'),
    ('src/table_fire.h', 'if (!g_skip.load(std::memory_order_acquire)) {\n    __imp__sub_82607778(ctx, base);\n    return;\n  }',
     'no skip wish: the original and nothing else'),
    ('src/table_fire.h', 'call(sub_826071F8, {ctrl});', 'the skip calls the hooked entry (the skipped-replay gate and the bus line run)'),
    ('src/table_fire.h', 'counts = PressCounts(call(sub_82564518, {ch}) != 0, call(sub_8256BD30, {ch}) != 0, call(sub_825FCBF0, {ch}) != 0);',
     'the fire hook asks the chooser\'s own press rule before it presses (review)'),
    ('src/table_fire.h', 'inline const bool g_states_registered = AddHostStateSource(&LookupState, &ReadState);', 'the table_fire_S states are registered'),
    ('src/replay_fx_skip.h', 'REX_HOOK_RAW(sub_826071F8)', 'replay_fx_skip.h still hooks the skip (the call reaches its gate)'),
    ('src/replay_fx_skip.h', 'if (wwe2k14::table_fire::ExpireSkip()) {', 'a replay end reports a skip wish it finds still armed (fix 2, run 318)'),
    ('src/situation_fire_verb.h', 'if (e.Ok(p) && ::wwe2k14::table_fire::ReplayLocked(e.U32(p + ::wwe2k14::table_fire::kReplayLockOff))) {',
     'skip_replay is refused while the game\'s replay lock word is set (fix 2)'),
    ('src/table_fire_core.h', '#include "chooser_press_core.h"', 'the fire core takes the shared press rule (fix 2)'),
    ('src/omg_wish_core.h', '#include "chooser_press_core.h"', 'the OMG core takes the shared press rule (fix 2)'),
    ('src/situation_director.h', '#include "table_fire.h"', 'situation_director.h includes table_fire.h (its one includer)'),
    ('src/situation_director.h', 'table_fire::Clear();', 'the wishes are cleared outside a match'),
    ('src/situation_director_core.h', '#include "table_fire_core.h"', 'the core includes the store (outside its namespace)'),
    ('src/situation_director_core.h', '#include "situation_fire_verb.h"', 'the core includes the verbs (inside its namespace)'),
    ('src/situation_director_core.h', 'case Verb::Fire: return FireTable(e, c.slot);', 'the director routes `fire SLOT`'),
    ('src/situation_director_core.h', 'if (c.verb == Verb::SkipReplay) return SkipReplay(e);', 'skip_replay is served before the match gate (a replay plays after the finish)'),
    ('src/situation_director_core.h', 'first == "fire" || first == "skip_replay"', 'both are director verbs (IsVerb)'),
]


def check(ok, msg):
    if not ok:
        problems.append(msg)
        print('FAIL: ' + msg)


def clang():
    for c in (shutil.which('clang++'), r'C:\Program Files\LLVM\bin\clang++.exe', '/usr/bin/clang++'):
        if c and os.path.exists(c):
            return c
    raise RuntimeError('no clang++ found (tools\\env.ps1 puts the project\'s LLVM on PATH)')


def read(rel):
    with open(os.path.join(REPO, *rel.split('/')), encoding='utf-8') as f:
        return f.read()


def build_run(inc, tmp, tag):
    exe = os.path.join(tmp, tag + '.exe')
    r = subprocess.run([clang(), '-std=c++17', '-O0', '-I', inc, '-I', os.path.join(REPO, 'src'), os.path.join(HERE, 'table_fire_test.cpp'), '-o', exe],
                       capture_output=True, text=True, timeout=240)
    if r.returncode != 0:
        return None, 'compile failed: ' + r.stderr[-1500:]
    r = subprocess.run([exe], capture_output=True, text=True, timeout=60)
    return r.returncode, r.stdout.replace('\r\n', '\n').strip()


def main():
    tmp = tempfile.mkdtemp(prefix='table_fire_')
    try:
        code, out = build_run(os.path.join(REPO, 'src'), tmp, 'honest')
        check(code == 0, 'the unit fails on the shipped core:\n%s' % out)
        print(out.splitlines()[-1] if out else '(no output)')
        caught = 0
        for plant in PLANTS:   # (name, pattern, replacement[, file under src/: default the core]); the twin file shadows src/ (-I inc first)
            name, pat, rep = plant[:3]
            target = plant[3] if len(plant) > 3 else 'table_fire_core.h'
            core = read('src/' + target)
            hits = len(re.findall(pat, core))
            if hits != 1:
                check(False, 'plant %s: the pattern matches %d times in %s (must be 1: the plant is stale)' % (name, hits, target))
                continue
            inc = os.path.join(tmp, 'plant_' + name)
            os.makedirs(inc)
            with open(os.path.join(inc, target), 'w', encoding='utf-8') as f:
                f.write(re.sub(pat, lambda _m: rep, core, count=1))
            if target != 'table_fire_core.h':   # the core's own "..." include looks in ITS folder first: the core sits next to the twin
                with open(os.path.join(inc, 'table_fire_core.h'), 'w', encoding='utf-8') as f:
                    f.write(read('src/table_fire_core.h'))
            code, out = build_run(inc, tmp, 'plant_' + name)
            if code is None:
                check(False, 'plant %s does not compile: %s' % (name, out))
            elif code == 0:
                check(False, 'plant %s was NOT caught by the unit' % name)
            else:
                caught += 1
        print('planted twins caught: %d of %d' % (caught, len(PLANTS)))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    for rel, text, what in TEXT:
        check(read(rel).count(text) == 1, 'text check: %s (%r once in %s)' % (what, text, rel))
    srcs = [n for n in os.listdir(os.path.join(REPO, 'src')) if n.endswith('.h')]
    for fn in ('sub_825FB230', 'sub_82607778'):
        hooks = sum(read('src/' + n).count('REX_HOOK_RAW(%s)' % fn) for n in srcs)
        check(hooks == 1, 'REX_HOOK_RAW(%s) appears %d times in src/*.h (want 1)' % (fn, hooks))
    includers = [n for n in os.listdir(os.path.join(REPO, 'src')) if n.endswith(('.h', '.cpp')) and '#include "table_fire.h"' in read('src/' + n)]
    check(includers == ['situation_director.h'], 'table_fire.h (it defines two hooks) must have ONE includer, situation_director.h: %s' % includers)
    defs = sum(read('src/' + n).count('inline bool PressCounts(') for n in srcs)   # fix 2: the chooser's press rule lives once
    check(defs == 1, 'PressCounts is defined %d times in src/*.h (want 1: src/chooser_press_core.h)' % defs)
    print('table_fire selftest: %d problems' % len(problems))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
