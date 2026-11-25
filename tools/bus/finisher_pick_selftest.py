"""S110 FINPICK: the check of src/finisher_pick_core.h (the rule the hook of sub_825FDE00 applies to the CPU's finisher pair). No game.

    py -I tools/bus/finisher_pick_selftest.py

Compiles tools/bus/finisher_pick_test.cpp against src/ (clang++ -std=c++17) and runs it; then each PLANTED TWIN (a copy of the core with one
fault, in a temp folder) must make it fail. TEXT CHECKS pin the wiring: one hook of sub_825FDE00 in src/*.h, in src/finisher_pick.h; it runs the
original first when no wish is set; situation_director.h includes finisher_pick.h, maps the verb to WishNow and clears outside a match; the core
routes `finisher` to Finisher.
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

# (name, pattern, replacement): each must match exactly once in src/finisher_pick_core.h and make the unit FAIL
PLANTS = [
    ('hold_end_off_by_one', r'kHoldEnd = 58399;', 'kHoldEnd = 58398;'),
    ('hold_first_off_by_one', r'kHoldFirst = 58000;', 'kHoldFirst = 58001;'),
    ('none_not_262', r'kNone = 262;', 'kNone = 0;'),
    ('drops_swapped', r'e\.drop0 = f1 && !f0;', 'e.drop0 = f0 && !f1;'),
    ('nohold_is_hold', r'\(wish == kNoHold \? !IsHold\(id\) : true\)', '(wish == kNoHold ? IsHold(id) : true)'),
    ('signature_pair_touched', r'\|\| !IsFinisherPair\(slot0, slot1\)\) return e;', ') return e;'),
    ('any_still_counted', r"n \+= g_wish\[s\]\.load\(std::memory_order_relaxed\) != kAny \? 1 : 0;", 'n += 1;'),
    ('clear_keeps_wishes', r'    g_wish\[s\]\.store\(kAny, std::memory_order_release\);\n', '\n'),
    ('state_seat_nine', r"c < char\('0' \+ kSeats\)", "c <= char('0' + kSeats)"),
    ('move_owner_ignores_ctx80', r'return ctx80 != 0 \? other : ch;', 'return ch;'),
    # S110 TOOLFIT: the posture families
    ('family_fit_is_one', r'\? kFitFamily : 1;', '? 1 : 1;'),
    ('family_end_off_by_one', r'\{58350, 58399, "\+1CC0", 5\}', '{58350, 58398, "+1CC0", 5}'),
    ('free_hold_ignores_family', r'return IsHold\(id\) && FamilyOf\(id\) == nullptr;', 'return IsHold(id);'),
    ('first_family_from_58000', r'\{58100, 58149, "\+1C54", 3\}', '{58000, 58149, "+1C54", 3}'),
]
TEXT = [
    ('src/finisher_pick.h', 'REX_HOOK_RAW(sub_825FDE00)', 'the pair write is hooked in finisher_pick.h'),
    ('src/finisher_pick.h', 'if (g_wishes.load(std::memory_order_acquire) == 0) {\n    __imp__sub_825FDE00(ctx, base);\n    return;\n  }',
     'no wish: the original and nothing else'),
    ('src/finisher_pick.h', 'inline const bool g_states_registered = AddHostStateSource(&LookupState, &ReadState);', 'the finisher_fit_S states are registered'),
    ('src/finisher_pick.h', 'const uint32_t owner = MoveOwner(Rd(chooser + kCtxOtherFlag), ch, Rd(ch + kOtherOff));',
     'the hook reads the ids of the character whose moveset the chooser tests (sub_82605418 +70..+98)'),
    ('src/finisher_pick.h', 'call(sub_8253AD80, {owner, s0})', 'slot [r4+0] is looked up in the owner\'s moveset'),
    ('src/situation_director.h', '#include "finisher_pick.h"', 'situation_director.h includes finisher_pick.h (its one includer)'),
    ('src/situation_director.h', 'return finisher_pick::WishNow(call, slot, ch, wish);', 'the verb\'s glue is WishNow'),
    ('src/situation_director.h', 'finisher_pick::Clear();', 'the wishes are cleared outside a match'),
    ('src/situation_director_core.h', 'case Verb::Finisher: return Finisher(e, c.slot, c.value);', 'the director routes the verb'),
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
    r = subprocess.run([clang(), '-std=c++17', '-O0', '-I', inc, os.path.join(HERE, 'finisher_pick_test.cpp'), '-o', exe],
                       capture_output=True, text=True, timeout=240)
    if r.returncode != 0:
        return None, 'compile failed: ' + r.stderr[-1500:]
    r = subprocess.run([exe], capture_output=True, text=True, timeout=60)
    return r.returncode, r.stdout.replace('\r\n', '\n').strip()


def main():
    tmp = tempfile.mkdtemp(prefix='finisher_pick_')
    try:
        code, out = build_run(os.path.join(REPO, 'src'), tmp, 'honest')
        check(code == 0, 'the unit fails on the shipped core:\n%s' % out)
        print(out.splitlines()[-1] if out else '(no output)')
        core = read('src/finisher_pick_core.h')
        caught = 0
        for name, pat, rep in PLANTS:
            hits = len(re.findall(pat, core))
            if hits != 1:
                check(False, 'plant %s: the pattern matches %d times in the core (must be 1: the plant is stale)' % (name, hits))
                continue
            inc = os.path.join(tmp, 'plant_' + name)
            os.makedirs(inc)
            with open(os.path.join(inc, 'finisher_pick_core.h'), 'w', encoding='utf-8') as f:
                f.write(re.sub(pat, lambda _m: rep, core, count=1))
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
    hooks = sum(read('src/' + n).count('REX_HOOK_RAW(sub_825FDE00)') for n in os.listdir(os.path.join(REPO, 'src')) if n.endswith('.h'))
    check(hooks == 1, 'REX_HOOK_RAW(sub_825FDE00) appears %d times in src/*.h (want 1)' % hooks)
    includers = [n for n in os.listdir(os.path.join(REPO, 'src')) if n.endswith(('.h', '.cpp')) and '#include "finisher_pick.h"' in read('src/' + n)]
    check(includers == ['situation_director.h'], 'finisher_pick.h (it defines a hook) must have ONE includer, situation_director.h: %s' % includers)
    print('finisher_pick selftest: %d problems' % len(problems))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
