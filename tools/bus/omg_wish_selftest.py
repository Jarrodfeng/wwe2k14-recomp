"""S110 OMGVERB: the check of src/omg_wish_core.h (the rule the hook of sub_825FCEB8 applies to a wishing seat's finisher candidate). No game.

    py -I tools/bus/omg_wish_selftest.py

Compiles tools/bus/omg_wish_test.cpp against src/ (clang++ -std=c++17) and runs it; then each PLANTED TWIN (a copy of the core with one fault,
in a temp folder) must make it fail. TEXT CHECKS pin the wiring: one hook of sub_825FCEB8 in src/*.h, in src/omg_wish.h; it runs the original
first when no wish is armed; the candidate is put back when the result is not kept; situation_director.h includes omg_wish.h (its one includer),
maps the verb to WishNow and clears outside a match; the core routes `omg` to Omg. Exit 0 = all passed.
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

# (name, pattern, replacement): each must match exactly once in src/omg_wish_core.h and make the unit FAIL
PLANTS = [
    ('ring_act_wrong', r'kRingAct = 2101;', 'kRingAct = 2150;'),
    ('signature_candidate_pressed', r'return wish && kind == kFinisherKind && press == 0;', 'return wish && press == 0;'),
    ('press_over_a_press', r'kind == kFinisherKind && press == 0;', 'kind == kFinisherKind;'),
    ('keeps_any_zero', r'return result == 0 && move == kRingAct && req_kind == kRequestKind && req_state == kRequestState;', 'return result == 0;'),
    # S110 OMGVERB fix 2: the request, the follow-up's pass, the lost follow-up, the end of a request (run 315)
    ('keeps_without_the_request_kind', r' && req_kind == kRequestKind', ''),
    ('reversal_is_a_break', r'return reversed != 0 \? kReversed : kBreak;', 'return kBreak;'),
    ('break_on_any_result', r'if \(result == 0 && move == kRingAct\) return reversed', 'if (move == kRingAct) return reversed'),
    ('lost_in_the_same_pass', r'frame != request_frame && ', ''),
    ('lost_while_the_follow_up_waits', r' && !has_follow_up8;', ';'),
    ('end_keeps_looking', r'\n  g_wish\[seat\]\.store\(false, std::memory_order_release\);\n', '\n'),
    ('request_not_a_state', r'g_code\[seat\]\.store\(kRequested \+ 1, std::memory_order_release\);', 'g_code[seat].store(kArmed + 1, std::memory_order_release);'),
    ('undone_result_zero', r'return result == kNotOurs \? kNotOurs : kNoPress;', 'return result;'),
    ('stock_not_a_gate', r'g\.latch && g\.stock\)', 'g.latch)'),
    ('failed_gate_stores_wish', r'g_wish\[seat\]\.store\(fit == 1, std::memory_order_release\);', 'g_wish[seat].store(true, std::memory_order_release);'),
    ('state_not_offset', r'return g_code\[seat\]\.load\(std::memory_order_acquire\) - 1;', 'return g_code[seat].load(std::memory_order_acquire);'),
    ('told_never_set', r'return !g_told\[seat\]\.exchange\(true\);', 'return !g_told[seat].load();'),
    ('clear_keeps_codes', r'    g_code\[s\]\.store\(0, std::memory_order_release\);\n', '\n'),
    ('state_seat_nine', r"c < char\('0' \+ kSeats\)", "c <= char('0' + kSeats)"),
    # S110 OMGVERB review: the chooser's own press rule (sub_826056E0 +A4..+EC)
    ('press_through_a_follow_up', r' && !has_follow_up; \}', '; }', 'chooser_press_core.h'),   # S110 FIRETABLE fix 2: the rule is shared
    ('press_through_the_act_set', r'return !act_clears && ', 'return ', 'chooser_press_core.h'),
]
TEXT = [
    ('src/omg_wish.h', 'REX_HOOK_RAW(sub_825FCEB8)', 'the ring-break test is hooked in omg_wish.h'),
    ('src/omg_wish.h', 'if (g_wishes.load(std::memory_order_acquire) == 0) {\n    __imp__sub_825FCEB8(ctx, base);\n    return;\n  }',
     'no wish: the original and nothing else'),
    ('src/omg_wish.h', 'std::memcpy(scenario::Guest(cand), snap, kCandSize);', 'an undone press puts the candidate back'),
    ('src/omg_wish.h', 'ctx.r3.u64 = Unpressed(result);', 'an undone press returns the no-press result'),
    ('src/omg_wish.h', '!PressCounts(act_clears, special_clears, fra != 0)',
     'the hook applies the chooser\'s own press rule (act set, sub_8256BD30, an FRA follow-up) before it presses'),
    ('src/omg_wish.h', 'if (Keep(result, Rd(cand + kMoveOff), Rd(cand + kReqKindOff), Rd(cand + kReqStateOff))) {',
     'only the request (+92 2101, +28 4, +116 839) is kept'),
    ('src/omg_wish.h', 'if (press != 0 && fra8) {  // the follow-up\'s own pass: the game\'s test, watched',
     'the follow-up\'s own pass is watched, not pressed'),
    ('src/omg_wish.h', 'const State s = Classify(result, move, Rd(cand + kReversedOff));', 'the follow-up\'s pass is classified from the game\'s own result'),
    ('src/omg_wish.h', 'if (IsLost(state, g_req_frame[seat].load(std::memory_order_relaxed), frame, fra8)) {', 'a follow-up gone without its test is named'),
    ('src/omg_wish.h', 'inline const bool g_states_registered = AddHostStateSource(&LookupState, &ReadState);', 'the omg_fit_S states are registered'),
    ('src/omg_wish.h', 'REXLOG_INFO("[wwe2k14][omg] {}", RequestText(', 'the request line carries the probe text of tools/run/scene_moments.py OMG_PROBE'),
    ('src/situation_director.h', '#include "omg_wish.h"', 'situation_director.h includes omg_wish.h (its one includer)'),
    ('src/situation_director.h', 'return omg_wish::WishNow(call, slot, ch, on);', 'the verb\'s glue is WishNow'),
    ('src/situation_director.h', 'omg_wish::Clear();', 'the wishes are cleared outside a match'),
    ('src/situation_director_core.h', 'case Verb::Omg: return Omg(e, c.slot, c.on);', 'the director routes the verb'),
    ('tools/run/scene_moments.py', 'OMG_PROBE = "the follow-up\'s ring-break test (FRA kind 8, its own press)"',
     'the tool probes the exe for the fix-2 hook by its follow-up line'),
    ('src/omg_wish_core.h', '": the follow-up\'s ring-break test (FRA kind 8, its own press) "', 'the follow-up line holds the tool\'s probe text'),
    ('tools/run/scene_moments.py', "'state {o} CornerGroggy', 'wait_state omg_fit_{s} >=3 3600 1', 'assert_state omg_fit_{s} 3')",
     'the recipe waits for the break start (3), not the request (2)'),
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
    r = subprocess.run([clang(), '-std=c++17', '-O0', '-I', inc, '-I', os.path.join(REPO, 'src'), os.path.join(HERE, 'omg_wish_test.cpp'), '-o', exe],
                       capture_output=True, text=True, timeout=240)
    if r.returncode != 0:
        return None, 'compile failed: ' + r.stderr[-1500:]
    r = subprocess.run([exe], capture_output=True, text=True, timeout=60)
    return r.returncode, r.stdout.replace('\r\n', '\n').strip()


def main():
    tmp = tempfile.mkdtemp(prefix='omg_wish_')
    try:
        code, out = build_run(os.path.join(REPO, 'src'), tmp, 'honest')
        check(code == 0, 'the unit fails on the shipped core:\n%s' % out)
        print(out.splitlines()[-1] if out else '(no output)')
        caught = 0
        for plant in PLANTS:   # (name, pattern, replacement[, file under src/: default the core]); the twin file shadows src/ (-I inc first)
            name, pat, rep = plant[:3]
            target = plant[3] if len(plant) > 3 else 'omg_wish_core.h'
            core = read('src/' + target)
            hits = len(re.findall(pat, core))
            if hits != 1:
                check(False, 'plant %s: the pattern matches %d times in %s (must be 1: the plant is stale)' % (name, hits, target))
                continue
            inc = os.path.join(tmp, 'plant_' + name)
            os.makedirs(inc)
            with open(os.path.join(inc, target), 'w', encoding='utf-8') as f:
                f.write(re.sub(pat, lambda _m: rep, core, count=1))
            if target != 'omg_wish_core.h':   # the core's own "..." include looks in ITS folder first: the core sits next to the twin
                with open(os.path.join(inc, 'omg_wish_core.h'), 'w', encoding='utf-8') as f:
                    f.write(read('src/omg_wish_core.h'))
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
    hooks = sum(read('src/' + n).count('REX_HOOK_RAW(sub_825FCEB8)') for n in os.listdir(os.path.join(REPO, 'src')) if n.endswith('.h'))
    check(hooks == 1, 'REX_HOOK_RAW(sub_825FCEB8) appears %d times in src/*.h (want 1)' % hooks)
    includers = [n for n in os.listdir(os.path.join(REPO, 'src')) if n.endswith(('.h', '.cpp')) and '#include "omg_wish.h"' in read('src/' + n)]
    check(includers == ['situation_director.h'], 'omg_wish.h (it defines a hook) must have ONE includer, situation_director.h: %s' % includers)
    print('omg_wish selftest: %d problems' % len(problems))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
