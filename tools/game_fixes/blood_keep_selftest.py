"""BLD7 (S108): offline checks of the blood-keeping game fix (src/blood_keep_core.h, src/blood_keep.cpp). No game runs.

    py -I tools/game_fixes/blood_keep_selftest.py             the unit test (the core COMPILED with clang++ against a fake game), its planted twins, the wiring pins
    py -I tools/game_fixes/blood_keep_selftest.py --listing   pins of every address / offset the fix relies on against the generated listing of the game's functions
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
problems = []


def check(ok, msg):
    if not ok:
        problems.append(msg)
        print('  FAIL: ' + msg)


def find_clang():
    for c in (shutil.which('clang++'), r'C:\Program Files\LLVM\bin\clang++.exe', '/usr/bin/clang++'):
        if c and os.path.isfile(c):
            return c
    raise RuntimeError('no clang++ found (tools\\env.ps1 puts the project\'s LLVM on PATH): the blood_keep unit test cannot run')


def rd(rel):
    return open(os.path.join(REPO, *rel.split('/')), encoding='utf-8').read()


def build(header_text, tmp, tag):
    inc = os.path.join(tmp, tag)
    os.makedirs(inc, exist_ok=True)
    with open(os.path.join(inc, 'blood_keep_core.h'), 'w', encoding='utf-8') as f:
        f.write(header_text)
    exe = os.path.join(inc, 'blood_keep_test.exe')
    r = subprocess.run([find_clang(), '-std=c++17', '-I', inc, os.path.join(HERE, 'blood_keep_test.cpp'), '-o', exe], capture_output=True, text=True, timeout=180)
    if r.returncode != 0 or not os.path.isfile(exe):
        raise RuntimeError('blood_keep_test did not compile (%s): %s' % (tag, (r.stdout + r.stderr)[-600:]))
    return exe


def run_test(exe):
    r = subprocess.run([exe], capture_output=True, text=True, timeout=60)
    return r.returncode, r.stdout


# the planted twins: (name, regex, replacement); each must match the header exactly once and make the test FAIL
PLANTS = [
    ('gate_ignored', r'if \(!gate\) \{', 'if (false) {'),
    ('no_skip_line', r'    e\.Log\(line \+ " skipped: blood off"\);\n', ''),
    ('counts_before_reset', r'(  e\.Call\(Fn::ResetBlood, cclm, 0, 0\);[^\n]*\n)(  const uint32_t live0 = [^\n]*\n)', r'\2\1'),
    ('no_reset', r'  e\.Call\(Fn::ResetBlood, cclm, 0, 0\);[^\n]*\n', '\n'),
    ('rebuild_after', r'(  for \(int k = 1; k <= l4; \+\+k\) e\.Call\(Fn::SetPart, cbdc, uint32_t\(k\), 0\);\n)', r'\1  e.Call(Fn::ResetBlood, cclm, 1, 0);\n'),
    ('reset_after_sets', r'  e\.Call\(Fn::ResetBlood, cclm, 0, 0\);[^\n]*\n([^\n]*\n[^\n]*\n[^\n]*\n[^\n]*\n)', r'\1  e.Call(Fn::ResetBlood, cclm, 0, 0);\n'),
    ('single_level', r'for \(int k = 1; k <= l0; \+\+k\) e\.Call\(Fn::SetFace, cbdc, uint32_t\(k\), 0\);', 'if (l0) e.Call(Fn::SetFace, cbdc, uint32_t(l0), 0);'),
    ('single_level_part', r'for \(int k = 1; k <= l3; \+\+k\) e\.Call\(Fn::SetPart, cbdc, uint32_t\(k\), 1\);', 'if (l3) e.Call(Fn::SetPart, cbdc, uint32_t(l3), 1);'),
    ('levels_not_capped', r'return b > uint32_t\(kMaxLevel\) \? kMaxLevel : int\(b\);', 'return int(b);'),
    ('all_zero_not_a_stop', r'if \(l0 == 0 && l3 == 0 && l4 == 0\) \{', 'if (false) {'),
    ('no_map_stop', r'if \(!e\.Ok\(cclm\)\) \{', 'if (false) {'),
    ('part3_r5_swapped', r'e\.Call\(Fn::SetPart, cbdc, uint32_t\(k\), 1\);', 'e.Call(Fn::SetPart, cbdc, uint32_t(k), 0);'),
    ('wrong_face_offset', r'kFaceLevelOff = 1392;', 'kFaceLevelOff = 1396;'),
    ('wrong_gate_offset', r'kGateOff = 15229;', 'kGateOff = 15228;'),
    ('restore_ignores_regions', r'return e\.Ok\(cclm\) && e\.U32\(cclm \+ kMapRegionsOff\) != 0;', 'return e.Ok(cclm);'),
]


def unit_tests(tmp):
    header = rd('src/blood_keep_core.h')
    code, out = run_test(build(header, tmp, 'honest'))
    m = re.search(r'SUMMARY (\d+) passed, (\d+) failed', out)
    check(code == 0 and m and m.group(2) == '0', 'the unit test fails on the shipped header:\n' + out[-800:])
    print('  unit test: %s' % (m.group(0) if m else 'no summary'))
    caught = 0
    for name, pat, rep in PLANTS:
        hits = len(re.findall(pat, header))
        if hits != 1:
            check(False, 'plant %s: the pattern matches %d times in the header (must be exactly 1: the plant is stale)' % (name, hits))
            continue
        twin = re.sub(pat, rep, header, count=1)
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
            print('  plant %-24s caught by: %s' % (name, ', '.join(failed[:2]) or 'a crash (exit %d)' % code))
    print('  planted twins caught: %d of %d' % (caught, len(PLANTS)))


def wiring():
    glue, app, core = rd('src/blood_keep.cpp'), rd('src/wwe2k14_app.h'), rd('src/blood_keep_core.h')
    # EXEB-B1: the glue is a unit of its own: listed in WWE2K14_SOURCES (both builds, Android uses the same list) and no longer included by the app header
    cm = [ln.split('#', 1)[0].strip() for ln in rd('CMakeLists.txt').replace('\\', '/').splitlines()]   # code only: a commented-out entry does not count
    check('src/blood_keep.cpp' in cm,'CMakeLists.txt WWE2K14_SOURCES does not list src/blood_keep.cpp (the hooks would not be linked)')
    check('blood_keep.h' not in app and not os.path.exists(os.path.join(REPO, 'src', 'blood_keep.h')), 'blood_keep.h is back (the glue is src/blood_keep.cpp; a header would put the hooks in main.cpp again)')
    check('#pragma once' not in glue, 'blood_keep.cpp holds #pragma once (it is a unit, not a header)')
    for fn in ('sub_825D99C8', 'sub_833693C8', 'sub_825C1888'):
        check(('REX_HOOK_RAW(%s)' % fn) in glue and ('__imp__%s(ctx, base);' % fn) in glue, 'the hook of %s is missing or does not run the original' % fn)
        # the original runs BEFORE Regrow in each hook
        body = glue[glue.index('REX_HOOK_RAW(%s)' % fn):]
        body = body[:body.index('\n}\n')]
        check(body.index('__imp__' + fn) < body.index('Regrow('), 'in the %s hook Regrow does not come after the original' % fn)
        if fn == 'sub_825D99C8':  # the original clears Swet+60 at its end (0x825D9B3C): read after the call it is always 0 and the fix never runs
            check(body.index('kSwetBurstOff') < body.index('__imp__' + fn), 'in the sub_825D99C8 hook the burst flag (Swet+60) is not read BEFORE the original')
    check('#if' not in glue and 'REXCVAR' not in glue, 'blood_keep.cpp has a build condition or a cvar: the fix is on in every build with no toggle')
    check('departs from the original game on purpose' in glue.lower() and 'GAME_BUG_FIXES.md' in glue and 'GAME_BUG_FIXES.md' in core, 'the departure notice / pointer is missing at the site')
    check('call(sub_825C1A60, {r3, r4, r5})' in glue and 'call(sub_825C19B0, {r3, r4})' in glue and 'call(sub_827261C0, {r3, r4})' in glue, 'a call in the glue changed')
    check(os.path.isfile(os.path.join(REPO, 'docs', 'GAME_BUG_FIXES.md')), 'docs/GAME_BUG_FIXES.md is missing')
    for ren in ('liquid_plan.h', 'live_map_sweat.cpp'):
        check(ren not in glue and ren not in core, 'the fix names a renderer file (%s)' % ren)


LISTING = {   # address: [text that must be in the listing]
    '825D99C8': ['lwz r11,60(r28)', 'lwz r30,36(r28)', 'bl 0x827261c0', 'bl 0x82727f30'],
    '833693C8': ['lwz r3,2552(r31)', 'lwz r11,228(r3)', 'bl 0x82727f30'],
    '825C1888': ['lwz r3,36(r3)', 'b 0x82726db8'],
    '825C18A0': ['stw r26,32(r30)', 'lwz r3,3736(r11)'],
    '825D9850': ['stw r25,32(r31)'],
    '825C19B0': ['cmplwi cr6,r4,3', 'bl 0x827270b0', 'bl 0x82726bb0'],
    '825C1A60': ['cmplwi cr6,r4,3', 'bl 0x827270b0', 'bl 0x82726bb0'],
    '827261C0': ['clrlwi r6,r8,5'],
    '825A1F28': ['bl 0x827d7578', '15229(r3)'],
    '827D7578': ['lis r11,-31881', 'addi r3,r11,-21772'],   # r3 = 0x8376AAF4 = kG, the object the gate byte is read from
    '825A0F28': ['stb r4,1392(r31)', 'stb r4,1396(r31)', 'stb r4,1397(r31)'],
    '8231F968': ['bl 0x825c1888'],
    '823202F8': ['bl 0x825c1888'],
}


def listing():
    main = os.environ.get('RDR_MAIN_CHECKOUT') or (REPO if os.path.isdir(os.path.join(REPO, 'generated', 'default')) else os.path.abspath(os.path.join(REPO, '..', '..', '..')))
    files = sorted(glob.glob(os.path.join(main, 'generated', 'default', 'wwe2k14_recomp.*.cpp')))
    if not files:
        check(False, '--listing: no generated code under %s (this check needs the player\'s own recompiled output)' % main)
        return
    for addr, needles in LISTING.items():
        out = subprocess.run([sys.executable, '-I', os.path.join(main, 'tools', 'analysis', 'list_function.py'), addr], cwd=main, capture_output=True, text=True, timeout=120).stdout
        low = out.lower()
        if 'not found' in low or not low.strip():
            check(False, '--listing: sub_%s not in the generated code' % addr)
            continue
        for n in needles:
            check(n.lower() in low, '--listing: sub_%s no longer has "%s"' % (addr, n))
    # sub_825C1888 has exactly the two callers
    calls = 0
    for f in files:
        calls += len(re.findall(r'^\tsub_825C1888\(ctx, base\);', open(f, encoding='utf-8', errors='replace').read(), flags=re.M))
    check(calls == 2, '--listing: sub_825C1888 has %d callers, not 2' % calls)
    core = rd('src/blood_keep_core.h')
    for name, val in (('kG', 0x8376AAF4), ('kGateOff', 15229), ('kFaceLevelOff', 1392), ('kPart3LevelOff', 1396), ('kPart4LevelOff', 1397), ('kCbdcTag', 0x43424443),
                      ('kCclmTag', 0x43434C4D), ('kCompDataOff', 24), ('kCbdcOwnerOff', 32), ('kCbdcMapOff', 36), ('kCharListOff', 3736), ('kSwetOwnerOff', 32),
                      ('kSwetMapOff', 36), ('kSwetBurstOff', 60), ('kRestoreListOff', 2552), ('kMapRegionsOff', 228)):
        m = re.search(r'%s = (0x[0-9A-Fa-f]+|\d+);' % name, core)
        check(bool(m) and int(m.group(1), 0) == val, 'core %s is not %s' % (name, val))
    check(0x4343 == 17219 and (0x4343 << 16 | 0x4C4D) == 0x43434C4D, 'the CCLM tag arithmetic (19533 = 0x4C4D)')
    print('  listing pins: %d functions, %d texts, sub_825C1888 callers %d' % (len(LISTING), sum(len(v) for v in LISTING.values()), calls))


def main(argv):
    tmp = tempfile.mkdtemp(prefix='bld7_')
    try:
        if '--listing' in argv:
            listing()
        else:
            unit_tests(tmp)
            wiring()
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if problems:
        print('FAILED: %d problem(s)' % len(problems))
        return 1
    print('PASSED')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
