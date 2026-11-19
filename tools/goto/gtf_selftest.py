"""S103 lane GTF: offline checks that the 30 Years jump arrives in the game's real state (captions of the jumped era). No game runs.

    py -I tools/goto/gtf_selftest.py            the call-site filter (src/wm30_args.h COMPILED with clang++ and driven: acts at lr 0x827AA340,
                                                refuses 0x827AA3D0 and every other lr), the planted twin that acts at both (must FAIL the same
                                                cases), wiring pins on src/wm30_jump.h (raw hook on sub_82A9D890 calls the game first, no r4 rewrite,
                                                the sub_82A9DFB8 hook only sets g_fire_started)
    py -I tools/goto/gtf_selftest.py --listing  pins of the listing of sub_827AA2E8 as the build sees it (reads generated/, never printed)

Exit 0 = all passed. A check that cannot read its input fails loudly. Source of the claims: docs/agents/s103/GTW.md.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
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
    raise RuntimeError("no clang++ found (tools\\env.ps1 puts the project's LLVM on PATH): the filter test cannot run")


def build_test(header_text, tmp, tag):
    """Compiles tools/goto/gtf_test.cpp against `header_text` as src/wm30_args.h (a copy under tmp, so a planted twin never touches the tree)."""
    inc = os.path.join(tmp, tag)
    os.makedirs(inc, exist_ok=True)
    with open(os.path.join(inc, 'wm30_args.h'), 'w', encoding='utf-8') as f:
        f.write(header_text)
    exe = os.path.join(inc, 'gtf_test.exe')
    r = subprocess.run([find_clang(), '-std=c++17', '-I', inc, os.path.join(HERE, 'gtf_test.cpp'), '-o', exe], capture_output=True, text=True, timeout=180)
    if r.returncode != 0 or not os.path.isfile(exe):
        raise RuntimeError('gtf_test did not compile: %s' % (r.stdout + r.stderr)[-600:])
    return exe


# (return address, expected). 0x827AA340 = state 1 (the era read), 0x827AA3D0 = state 3 (the code read), the rest are other callers.
CASES = [
    ('827AA340', 'ACT'),
    ('827AA3D0', 'SKIP'),
    ('827AA360', 'SKIP'),   # the sub_82A9DFB8 return: not this getter's site
    ('827AA370', 'SKIP'),
    ('827AA33C', 'SKIP'),   # the call instruction, not its return
    ('00000000', 'SKIP'),
    ('82A9D890', 'SKIP'),
]


def run_cases(exe):
    r = subprocess.run([exe], input='\n'.join(c for c, _ in CASES) + '\n', capture_output=True, text=True, timeout=60)
    out = r.stdout.split('\n')
    if out and out[-1] == '':
        out.pop()
    if len(out) != len(CASES):
        raise RuntimeError('gtf_test answered %d lines for %d inputs' % (len(out), len(CASES)))
    return ['lr 0x%s: got %s, expected %s' % (c, o.strip(), e) for (c, e), o in zip(CASES, out) if o.strip() != e]


def read(*parts):
    return open(os.path.join(REPO, *parts), encoding='utf-8').read()


def between(t, a, b):
    i = t.index(a)
    return t[i:t.index(b, i + len(a))] if b in t[i + len(a):] else t[i:]


def units_main():
    header = read('src', 'wm30_args.h')
    with tempfile.TemporaryDirectory() as tmp:
        bad = run_cases(build_test(header, tmp, 'real'))
        check(not bad, 'the call-site filter disagrees: %s' % bad)
        # planted twins: each must be caught by the same cases
        twins = {
            'acts at both call sites (state 1 and state 3)': ('return lr == kSelReadRet;', 'return lr == kSelReadRet || lr == kSelReadRetState3;'),
            'acts at the state-3 site only': ('return lr == kSelReadRet;', 'return lr == kSelReadRetState3;'),
            'acts at every call site': ('return lr == kSelReadRet;', 'return true;'),
            'state-1 return address off by one insn': ('kSelReadRet = 0x827AA340;', 'kSelReadRet = 0x827AA344;'),
        }
        for what, (old, new) in twins.items():
            if old not in header:
                check(False, 'planted twin "%s" did not change the header (the test lost its anchor)' % what)
                continue
            bad = run_cases(build_test(header.replace(old, new), tmp, 'twin%d' % abs(hash(what))))
            check(bool(bad), 'planted twin (%s) was not caught' % what)
    print('  filter: %d cases pass; %d planted twins caught' % (len(CASES), len(twins)))

    jump = read('src', 'wm30_jump.h')
    hook = jump[jump.index('REX_HOOK_RAW(sub_82A9D890)'):jump.index('REX_HOOK_RAW(sub_82A9DFB8)')]
    old_hook = jump[jump.index('REX_HOOK_RAW(sub_82A9DFB8)'):]
    check('__imp__sub_82A9D890(ctx, base);' in hook, 'the getter hook does not call the game function')
    check(hook.index('__imp__sub_82A9D890(ctx, base);') < hook.index('IsSelectionReadSite(lr)') < hook.index('Put32(sel + 4'),
          'order: the game getter, then the lr filter, then the selection write')
    check('const uint32_t lr = uint32_t(ctx.lr);' in hook and hook.index('const uint32_t lr') < hook.index('__imp__sub_82A9D890'),
          'the lr is not read before the call')
    check('Put32(sel + 4, uint32_t(era))' in hook and 'Put32(sel + 8, uint32_t(code))' in hook, 'the hook does not write [r3+4] = era and [r3+8] = code')
    check('ctx.r4' not in jump, 'a ctx.r4 rewrite is back in wm30_jump.h (the game\'s own lwz r4,4(r3) must feed both loads)')
    check('ctx.r30' not in jump, 'a hand-set r30 is back (rejected in GTW.md section 4)')
    check('kStartPathRet' in old_hook and 'g_fire_started.store(true)' in old_hook and 'Put32' not in old_hook and 'g_live_arm' not in old_hook,
          'the sub_82A9DFB8 hook must only set g_fire_started')
    check('REX_EXTERN(__imp__sub_82A9D890)' in jump, 'the getter __imp__ is not declared')
    check(jump.count('REX_HOOK_RAW(sub_82A9D890)') == 1, 'two hooks of sub_82A9D890 in the file')
    print('  wiring pins checked')


def listing_main():
    gen = os.path.join(os.environ.get('RDR_MAIN_CHECKOUT') or resolve.main_checkout(), 'generated', 'default')
    if not os.path.isdir(gen):
        raise RuntimeError('no generated code at %s' % gen)
    text = None
    for fn in sorted(os.listdir(gen)):
        if not fn.endswith('.cpp'):
            continue
        t = open(os.path.join(gen, fn), encoding='utf-8', errors='replace').read()
        i = t.find('DEFINE_REX_FUNC(sub_827AA2E8)')
        if i >= 0:
            j = t.find('\nDEFINE_REX_FUNC', i + 10)
            text = t[i:j if j > 0 else len(t)]
            break
    if text is None:
        raise RuntimeError('sub_827AA2E8 not found under %s' % gen)
    t = re.sub(r'[ \t]+', ' ', text)

    def after(marker, n=1):
        i = t.index(marker)
        return t[i:i + 2500]
    s1 = after('ctx.lr = 0x827AA340;')
    s3 = after('ctx.lr = 0x827AA3D0;')
    pins = [
        ('state 1: bl sub_82A9D890 returns at 0x827AA340', re.search(r'ctx\.lr = 0x827AA340;\s*sub_82A9D890\(ctx, base\);', t) is not None),
        ('state 1: the era is read by lwz r4,4(r3) right after the getter', re.match(r'ctx\.lr = 0x827AA340;\s*sub_82A9D890\(ctx, base\);\s*// lwz r4,4\(r3\)\s*ctx\.r4\.u64 = REX_LOAD_U32\(ctx\.r3\.u32 \+ 4\);', s1) is not None),
        ('state 1: r30 = (era == 5 ? 0 : era) (addi -5, subfic 0, subfe, and r30,r9,r4)',
         all(k in s1 for k in ('ctx.r11.u64 = ctx.r4.u64 + uint64_t(int64_t(-5));', 'xer.ca = ctx.r11.u32 <= 0;', 'ctx.r30.u64 = ctx.r9.u64 & ctx.r4.u64;'))),
        ('state 1: sub_82A9DFB8 (return 0x827AA360) then sub_82A9E7B8 with r4 = r30 (return 0x827AA370)',
         s1.index('ctx.lr = 0x827AA360;') < s1.index('ctx.r4.u64 = ctx.r30.u64;') < s1.index('ctx.lr = 0x827AA370;') and 'sub_82A9E7B8(ctx, base);' in s1),
        ('state 1: the caption table slot is [r31+20] (S+20)', 'ctx.r3.u64 = REX_LOAD_U32(ctx.r31.u32 + 20);' in s1[:s1.index('ctx.lr = 0x827AA370;')]),
        ('state 3: bl sub_82A9D890 returns at 0x827AA3D0 and reads the code with lwz r28,8(r3)',
         re.match(r'ctx\.lr = 0x827AA3D0;\s*sub_82A9D890\(ctx, base\);\s*// lwz r28,8\(r3\)\s*ctx\.r28\.u64 = REX_LOAD_U32\(ctx\.r3\.u32 \+ 8\);', s3) is not None),
        ('only these two calls of sub_82A9D890 are in the machine', t.count('sub_82A9D890(ctx, base);') == 2),
    ]
    for what, ok in pins:
        check(ok, 'listing pin failed: %s' % what)
    print('  listing pins: %d checked against sub_827AA2E8' % len(pins))


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else ''
    try:
        if mode == '--listing':
            listing_main()
        else:
            units_main()
    except (RuntimeError, ValueError) as x:
        check(False, 'cannot read an input: %s' % x)
    if problems:
        print('GTF selftest: %d problem(s)' % len(problems))
        sys.exit(1)
    print('GTF selftest: ok')


if __name__ == '__main__':
    main()
