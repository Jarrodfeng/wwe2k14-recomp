"""S103 lane WMB: offline checks of the menu-free 30 Years start (--wwe2k14_wm30_direct). No game runs.

    py -I tools/wm30/boot_selftest.py            the order of steps (src/wm30_boot_guard.h COMPILED with clang++ and driven by tools/wm30/boot_test.cpp
                                                 with a recording stand-in for the game: 13 scenarios whose exact traces are below), the planted twins
                                                 (each must change at least one scenario), text pins on the real calls (src/wm30_boot.h), the wiring
                                                 (owner update only, never the VdSwap hook; cvar default off; menu drive skipped when direct), and
                                                 the twins of those pins
    py -I tools/wm30/boot_selftest.py --listing  pins of the listings of every link of the chain, as the build sees them (reads generated/, never printed)

Exit 0 = all passed. A check that cannot read its input fails loudly. Source of the claims: docs/agents/s103/WMD.md, WMD_ADV.md (C1-C7).
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


def check(ok, msg):
    if not ok:
        problems.append(msg)
        print('  FAIL: ' + msg)


def find_clang():
    for c in (shutil.which('clang++'), r'C:\Program Files\LLVM\bin\clang++.exe', '/usr/bin/clang++'):
        if c and os.path.isfile(c):
            return c
    raise RuntimeError("no clang++ found (tools\\env.ps1 puts the project's LLVM on PATH): the order test cannot run")


def read(*parts):
    return open(os.path.join(REPO, *parts), encoding='utf-8').read()


def build_and_run(header_text, tmp, tag):
    """Compiles tools/wm30/boot_test.cpp against `header_text` as wm30_boot_guard.h (a copy under tmp, so a twin never touches the tree)."""
    inc = os.path.join(tmp, tag)
    os.makedirs(inc, exist_ok=True)
    with open(os.path.join(inc, 'wm30_boot_guard.h'), 'w', encoding='utf-8') as f:
        f.write(header_text)
    exe = os.path.join(inc, 'boot_test.exe')
    r = subprocess.run([find_clang(), '-std=c++17', '-I', inc, os.path.join(HERE, 'boot_test.cpp'), '-o', exe], capture_output=True, text=True, timeout=180)
    if r.returncode != 0 or not os.path.isfile(exe):
        raise RuntimeError('boot_test did not compile: %s' % (r.stdout + r.stderr)[-600:])
    out = subprocess.run([exe], capture_output=True, text=True, timeout=120)
    return [l for l in out.stdout.split('\n') if l]


# scenario|trace|facts. Frames: idle from 0, so the 60th idle frame is frame 59; ready_after 5 -> ready at 64.
EXPECTED = [
    # the happy path: pre-creates and S under heap 0, wait for ready, decode, reset, select, exit action 3, clear the flag
    'happy|pre(0) create(0) decode reset select@ready exit(3)@ready finished|first=59 last=64 exits=1',
    # S already exists: no create; nothing is destroyed later
    'existing_s|pre(0) decode reset select@ready exit(3)@ready finished|first=59 last=60 exits=1',
    # match N not in the index while ready: never exit; timeout (600 waited) destroys S (ready, so a clean destroy)
    'lookup_miss_ready|pre(0) create(0) destroy@ready|first=59 last=259 exits=0',
    # the reads never finish: never exit; timeout destroys mid-read (the only mid-read destroy)
    'never_ready|pre(0) create(0) destroy@pending|first=59 last=259 exits=0',
    # idle lost before ready (C5): wait for ready, then destroy; never destroy mid-read at once
    'idle_lost_before_ready|pre(0) create(0) destroy@ready|first=59 last=109 exits=0',
    # idle lost and the reads never finish: destroy after the bounded abort wait (63 + 300 = frame 363)
    'idle_lost_never_ready|pre(0) create(0) destroy@pending|first=59 last=163 exits=0',
    # idle lost after the selection was written (C6): no exit, S destroyed
    'idle_lost_after_select|pre(0) create(0) decode reset select@ready destroy@ready|first=59 last=64 exits=0',
    # one non-idle frame resets the idle count: 60 more frames
    'idle_gap_at_59|pre(0) create(0) decode reset select@ready exit(3)@ready finished|first=119 last=124 exits=1',
    # the profile / game data objects do not exist yet: nothing until they do
    'prereq_late|pre(0) create(0) decode reset select@ready exit(3)@ready finished|first=100 last=105 exits=1',
    # the same frame number three times counts once (the tick may run more than once per frame)
    'same_frame_thrice|pre(0) create(0) decode reset select@ready exit(3)@ready finished|first=59 last=64 exits=1',
    # an S this machine did not create is never destroyed by it
    'existing_s_idle_lost|pre(0)|first=59 last=59 exits=0',
    # ready but the match is not in the index and idle is lost: destroy at once (the reads are done)
    'lookup_miss_idle_lost|pre(0) create(0) destroy@ready|first=59 last=70 exits=0',
    # S replaced / the sequence left / sequence 31 adopted S (exit action 2 or 4) while an abort waits: never destroyed here
    'abort_s_replaced|pre(0) create(0)|first=59 last=59 exits=0',
    'abort_seq_left|pre(0) create(0)|first=59 last=59 exits=0',
    'abort_adopted_action2|pre(0) create(0)|first=59 last=59 exits=0',
    'abort_adopted_action4_later|pre(0) create(0)|first=59 last=59 exits=0',
    # an existing S whose match is not in the index: the timeout (20 s = frame 259) never destroys an S the machine did not create
    'existing_s_lookup_miss|pre(0)|first=59 last=59 exits=0',
    # [S+4] missing: abort, S destroyed (reads done), nothing written
    'no_selection_object|pre(0) create(0) destroy@ready|first=59 last=64 exits=0',
    # wall time: 1 s per frame times out 20 s after the wait began (frame 79); 10 ms per frame still waits for ready at frame 1559 (15.6 s)
    'slow_clock_timeout|pre(0) create(0) destroy@pending|first=59 last=79 exits=0',
    'fast_clock_waits_long|pre(0) create(0) decode reset select@ready exit(3)@ready finished|first=59 last=1559 exits=1',
    # never idle: the machine never acts
    'never_idle||first=-1 last=-1 exits=0',
]


def order_main():
    header = read('src', 'wm30_boot_guard.h')
    with tempfile.TemporaryDirectory() as tmp:
        got = build_and_run(header, tmp, 'real')
        check(got == EXPECTED, 'the order test disagrees:\n' + '\n'.join('      got      %s\n      expected %s' % (g, e) for g, e in zip(got, EXPECTED) if g != e)
              + ('' if len(got) == len(EXPECTED) else '\n      %d lines for %d scenarios' % (len(got), len(EXPECTED))))
        # planted twins of the header: each must change at least one scenario's line
        twins = {
            'exit before ready (the ready test dropped)': ('if (g.Ready() && g.Lookup(era, code)) {', 'if (g.Lookup(era, code)) {'),
            'exit on a failed lookup (the lookup dropped)': ('if (g.Ready() && g.Lookup(era, code)) {', 'if (g.Ready()) {'),
            'exit action 2 (opens the 30 Years menus)': ('kExitAction = 3;', 'kExitAction = 2;'),
            'exit action 4': ('kExitAction = 3;', 'kExitAction = 4;'),
            'selection before the ready wait': ('      m.created = true;', '      m.created = true;\n      g.Select(0, 0);'),
            'heap 8 for S (sequence 31\'s heap)': ('kCreateHeap = 0;', 'kCreateHeap = 8;'),
            'abort without destroy': ('if (Owned(m, g)) g.Destroy();', 'if (Owned(m, g)) ;'),
            'destroys an S that is not ours any more (replaced / sequence left)': ('return m.created && m.s_addr != 0 && g.SAddr() == m.s_addr && g.MainMenuSeq();', 'return m.created;'),
            'ignores that the sequence left the main menu': (' && g.MainMenuSeq();', ';'),
            'ignores that S was replaced': ('g.SAddr() == m.s_addr && ', ''),
            'destroys after sequence 31 adopted S (action 2 / 4)': ('  return action == 2 || action == 4;', '  return false;'),
            'timeout destroys an S it did not create': ('    if (m.created) DestroyOwned(m, g);\n    else m.phase = Phase::kFailed;', '    g.Destroy();\n    m.phase = Phase::kFailed;'),
            'ready timeout in frames, not wall time': ('g.NowMs() - m.wait_start_ms >= kReadyTimeoutMs', 'm.waited >= 600'),
            'abort-ready timeout in frames, not wall time': ('g.NowMs() - m.wait_start_ms >= kAbortReadyTimeoutMs', 'm.waited >= 300'),
            'selection written without the progress object test': ('if (!g.SelectionObject()) {', 'if (false) {'),
            'no idle re-check after the selection (C6)': ('if (!g.Idle()) {  // C6', 'if (false) {  // C6'),
            'abort destroys mid-read at once (C5)': ('  if (g.Ready()) {\n    DestroyOwned(m, g);', '  if (true) {\n    DestroyOwned(m, g);'),
            'no idle streak (a gap does not reset the count)': ('m.idle = g.Idle() ? m.idle + 1 : 0;', 'm.idle = m.idle + 1;'),
            'no once-per-frame guard': ('if (frame == m.last_frame) return;', ''),
            'decode (C3) dropped': ('g.DecodeKinds();', ''),
            'match description reset (C4) dropped': ('g.ResetMatchDescription();', ''),
            'no prerequisites test': ('if (m.idle < kIdleFrames || !g.Prerequisites()) return;', 'if (m.idle < kIdleFrames) return;'),
            'no timeout': ('if (g.NowMs() - m.wait_start_ms >= kReadyTimeoutMs) {', 'if (false) {'),
            'progress flag cleared before the exit': ('    g.Exit(kExitAction);\n    g.Finished();', '    g.Finished();\n    g.Exit(kExitAction);'),
        }
        for what, (old, new) in twins.items():
            if old not in header:
                check(False, 'planted twin "%s" did not change the header (the test lost its anchor)' % what)
                continue
            try:
                twin_got = build_and_run(header.replace(old, new), tmp, 'twin%d' % (abs(hash(what)) % 10 ** 8))
            except RuntimeError as x:
                check(False, 'planted twin "%s" did not compile: %s' % (what, x))
                continue
            check(twin_got != EXPECTED, 'planted twin (%s) was not caught' % what)
    print('  order: %d scenarios pass; %d planted twins caught' % (len(EXPECTED), len(twins)))


def between(t, a, b):
    i = t.index(a)
    j = t.index(b, i + len(a)) if b in t[i + len(a):] else len(t)
    return t[i:j]


def wiring_problems(boot, scenario, jump, kernel):
    """The text pins on the real calls and the wiring; returns a list of failed pin names (empty = all hold)."""
    bad = []

    def pin(ok, what):
        if not ok:
            bad.append(what)
    # the tick runs in the owner update, never on the VdSwap frame hook
    pin('g_wm30_frame_hook' not in boot and 'OnFrame' not in boot, 'the boot tick touches the VdSwap frame hook')
    pin('g_wm30_boot_tick' not in kernel, 'the VdSwap path (kernel_overrides.h) calls the boot tick')
    hook = between(scenario, 'REX_HOOK_RAW(sub_82C302A0) {', '\n}\n')
    pin('g_wm30_boot_tick(ctx, base)' in hook, 'the owner update (sub_82C302A0 hook) does not call g_wm30_boot_tick')
    pin(scenario.count('g_wm30_boot_tick(ctx, base)') == 1, 'g_wm30_boot_tick is called from more than one place')
    pin('__imp__sub_82C302A0(ctx, base)' in hook and 'g_wm30_boot_tick(ctx, base)' in hook and hook.index('__imp__sub_82C302A0(ctx, base)') < hook.index('g_wm30_boot_tick(ctx, base)'), 'the boot tick runs BEFORE the original owner update (a press of this frame would not be seen)')
    pin('g_wm30_boot_tick = &Tick' in boot, 'the tick is not registered')
    # no script variable 215 (native 8 sets it), no progress load (C1), no mid-air selection writes
    pin('sub_82A9CAD8' not in boot, 'a script variable is written (sub_82A9CAD8; var 215 is native 8\'s)')
    pin('sub_82A9D750' not in boot, 'the progress load sub_82A9D750 is back (C1: dropped)')
    pin('Put32(' in boot and boot.count('Put32(') == 1 and 'Put32(b + wm30::kSelection + 8, uint32_t(code))' in boot, 'the code is not the only hand-written word (B + 2352)')
    # the chain of real calls
    sel = between(boot, 'void Select(', '// C7 / the pad')
    pin(sel.index('sub_82A9D9F8') < sel.index('Put32('), 'the selection: sub_82A9D9F8 must come before B+2352 = code (it writes 10000 there)')
    pin('{b, uint64_t(era), pad}' in sel, 'sub_82A9D9F8 is not called with (B, era, pad)')
    pin('sub_82A9D9F8' in sel and 'sub_82A9F208' in sel and sel.index('sub_82A9D9F8') < sel.index('sub_82A9DB50') < sel.index('sub_82A9F208') < sel.index('Put32('), 'WMS: the picture table fill (sub_82A9DB50 rows, sub_82A9F208 per row) must sit between sub_82A9D9F8 and the B+2352 write')
    pin('call(sub_82A9F208, {a, call(sub_82A9DB90, {astl, 4, uint64_t(r), 3}), call(sub_82A9DB90, {astl, 4, uint64_t(r), 4})})' in sel and 'int16_t(call(sub_82A9DB50, {astl, 4}))' in sel and 'astl = scenario::U32(s + 16), a = scenario::U32(s + 8)' in sel, 'WMS: the fill is not sub_8284E1E0 +4B0..+4D0 (A = [S+8], ASTL = [S+16], kind 4, columns 3 and 4)')
    pin('const uint32_t pad = p ? scenario::U32(p) : 0;' in sel, 'the pad is not [P] (p is already [0x8376AAF0]; one pointer, not two)')
    pin('p ? int32_t(U32(p)) : -1' in boot, 'the route log pad is not [P]')
    pin('steady_clock::now()' in boot, 'the timeouts are not wall-clock (steady_clock)')
    pin('U32(wm30::kModeState)' in between(boot, 'uint32_t SAddr()', 'bool SelectionObject()') and 'kMenuSelection' in between(boot, 'uint32_t ExitAction()', 'uint32_t SAddr()') and 'kSeqMainMenu' in between(boot, 'bool MainMenuSeq()', 'uint32_t ExitAction()') and 's + 4) != 0' in between(boot, 'bool SelectionObject()', 'bool Prerequisites()'), 'the ownership / adoption / progress-object reads are not the S word, 0x83929434, sequence 75 and [S+4]')
    pin('wm30::kSelection + 8' in sel, 'the code word is not B + 2344 + 8')
    pin('call(sub_8283CFF0, {scenario::U32(scenario::kSmartMenuPtr), uint64_t(action)})' in boot, 'the exit does not pass (SmartMenu, the guard\'s action)')
    cs = between(boot, 'void CreateS(', 'bool Ready()')
    pin('sub_82C2DE28, {heap}' in cs and 'sub_828265C0' in cs and cs.index('{heap}') < cs.index('sub_828265C0') < cs.index('sub_82C2DE28, {h}'), 'S is not created inside the heap bracket (heap parameter, then sub_828265C0, then the old heap back)')
    pc = between(boot, 'void PreCreate(', 'void CreateS(')
    pin('sub_82C2DE28, {heap}' in pc and 'kPreCreate1) == 0' in pc and 'kPreCreate2) == 0' in pc, 'the pre-creates are not heap-bracketed and null-checked')
    rm = between(boot, 'void ResetMatchDescription(', '// Step 6')
    pin('sub_82C2DE28, {scenario::U32(kHeapWord)}' in rm and rm.count('sub_827D7368') == 2 and 'i < 43' in rm and '+ 45)[0] = uint8_t(i)' in rm,
        'the match description reset (C4) is not the controller\'s (heap word, 43 rows, +45 = i)')
    pin('sub_82A9E0C8' in boot and '+ 16' in between(boot, 'void DecodeKinds(', '// Step 5'), 'C3: sub_82A9E0C8([S+16]) is missing')
    pin('wwe2k14_wm30_direct' in between(boot, 'inline void Tick(', 'inline const bool g_registered'), 'the tick does not test --wwe2k14_wm30_direct')
    pin('g_boot_pending.store(false)' in boot and 'void Finished()' in boot, 'Finished does not clear the boot-pending flag')
    ex = between(boot, 'bool Idle() {', 'bool Prerequisites()')
    pin('kSmartMenuPtr' in ex and 'kSeqMainMenu' in ex and 'kMenuSelection' in ex and 'sm + 364' in ex, 'the idle test is not the launcher\'s')
    # the cvar and the old drive
    pin(re.search(r'REXCVAR_DEFINE_BOOL\(wwe2k14_wm30_direct,\s*false,', jump) is not None, 'wwe2k14_wm30_direct is not a bool defaulting to false')
    onf = between(jump, 'inline void OnFrame(uint64_t frame) {', '  g_last_frame = frame;')
    pin('REXCVAR_GET(wwe2k14_wm30_direct)' in onf, 'the menu drive (OnFrame) does not stand down when direct is on')
    pin('kDrive' in jump and 'press_until_state DOWN 0x8392945C>352' in jump, 'the menu drive (inline chains) is gone: the off path must be today\'s')
    pin('REX_HOOK_RAW(sub_82A9D890)' in jump, 'the getter hook fallback is gone')
    return bad


def wiring_main():
    boot, scenario, jump, kernel = read('src', 'wm30_boot.h'), read('src', 'scenario.h'), read('src', 'wm30_jump.h'), read('src', 'kernel_overrides.h')
    bad = wiring_problems(boot, scenario, jump, kernel)
    check(not bad, 'wiring pins failed: %s' % bad)
    twins = {
        'var 215 written (sub_82A9CAD8)': ('boot', 'void Finished()', 'void Junk() { Caller call(ctx, base); call(sub_82A9CAD8, {215, 1}); }\n  void Finished()'),
        'the progress load is back': ('boot', 'void Finished()', 'void Junk() { Caller call(ctx, base); call(sub_82A9D750, {0}); }\n  void Finished()'),
        'running on the VdSwap frame hook': ('boot', 'inline Machine g_machine;', 'inline Machine g_machine;\ninline const bool g_vdswap = (g_wm30_frame_hook = nullptr, true);'),
        'the VdSwap path calls the tick': ('kernel', 'g_wm30_frame_hook(g_guest_frame);', 'g_wm30_frame_hook(g_guest_frame); g_wm30_boot_tick(ctx, base);'),
        'the tick moved out of the owner update': ('scenario', '  if (wwe2k14::scenario::g_wm30_boot_tick) wwe2k14::scenario::g_wm30_boot_tick(ctx, base);', '  if (wwe2k14::scenario::g_wm30_boot_tick) wwe2k14::scenario::g_wm30_boot_tick(ctx, base);'.replace('g_wm30_boot_tick(ctx, base)', 'g_wm30_boot_tick_x(ctx, base)')),
        'the selection written before the pick function': ('boot', '    call(sub_82A9D9F8, {b, uint64_t(era), pad});\n', '    gfx_menu::Put32(b + wm30::kSelection + 8, uint32_t(code));\n    call(sub_82A9D9F8, {b, uint64_t(era), pad});\n'),
        'S created under a fixed heap 8': ('boot', 'call(sub_82C2DE28, {heap});\n    call(sub_828265C0, {});', 'call(sub_82C2DE28, {8});\n    call(sub_828265C0, {});'),
        'exit action fixed at 2': ('boot', 'uint64_t(action)})', '2})'),
        'default on': ('jump', 'wwe2k14_wm30_direct, false,', 'wwe2k14_wm30_direct, true,'),
        'the menu drive does not stand down': ('jump', ' || REXCVAR_GET(wwe2k14_wm30_direct) || frame == g_last_frame', ' || frame == g_last_frame'),
        'the menu drive removed': ('jump', 'press_until_state DOWN 0x8392945C>352', 'press_until_state DOWN X'),
        'the tick does not test the cvar': ('boot', '  if (!REXCVAR_GET(wwe2k14_wm30_direct)) return;\n', ''),
        'the pad is one pointer too deep (select)': ('boot', 'const uint32_t pad = p ? scenario::U32(p) : 0;', 'const uint32_t pad = p ? scenario::U32(scenario::U32(p)) : 0;'),
        'the pad is one pointer too deep (log)': ('boot', 'p ? int32_t(U32(p)) : -1', 'p ? int32_t(U32(U32(p))) : -1'),
        'the picture table fill dropped': ('boot', '      call(sub_82A9F208, {a, call(sub_82A9DB90, {astl, 4, uint64_t(r), 3}), call(sub_82A9DB90, {astl, 4, uint64_t(r), 4})});', '      (void)r;'),
        'frames as the clock': ('boot', 'steady_clock::now()', 'system_clock::now()'),
        'the boot tick before the original update': ('scenario', '  __imp__sub_82C302A0(ctx, base);\n', '  if (wwe2k14::scenario::g_wm30_boot_tick) wwe2k14::scenario::g_wm30_boot_tick(ctx, base);\n  __imp__sub_82C302A0(ctx, base);\n'),
        'S ownership reads the wrong word': ('boot', 'uint32_t SAddr() { return scenario::U32(wm30::kModeState); }', 'uint32_t SAddr() { return 1; }'),
        'adoption reads the wrong word': ('boot', 'uint32_t ExitAction() { return scenario::U32(scenario::kMenuSelection); }', 'uint32_t ExitAction() { return 0; }'),
        'the match description reset lost its heap': ('boot', 'call(sub_82C2DE28, {scenario::U32(kHeapWord)});', ''),
    }
    src = {'boot': boot, 'scenario': scenario, 'jump': jump, 'kernel': kernel}
    for what, (which, old, new) in twins.items():
        if old not in src[which]:
            check(False, 'planted twin "%s" did not change %s (the test lost its anchor)' % (what, which))
            continue
        mod = dict(src)
        mod[which] = src[which].replace(old, new)
        check(bool(wiring_problems(mod['boot'], mod['scenario'], mod['jump'], mod['kernel'])), 'planted wiring twin (%s) was not caught' % what)
    print('  wiring: pins hold; %d planted twins caught' % len(twins))


# ---- listing pins ----

def find_function(gen, name):
    for fn in sorted(os.listdir(gen)):
        if not fn.endswith('.cpp'):
            continue
        t = open(os.path.join(gen, fn), encoding='utf-8', errors='replace').read()
        i = t.find('DEFINE_REX_FUNC(%s)' % name)
        if i >= 0:
            j = t.find('\nDEFINE_REX_FUNC', i + 10)
            body = t[i:j if j > 0 else len(t)]
            body = re.sub(r'//[^\n]*', '', body)   # drop the listing comments
            return re.sub(r'\s+', ' ', body)
    raise RuntimeError('%s not found under %s' % (name, gen))


def listing_main():
    sys.path.insert(0, os.path.join(REPO, 'tools', 'goto'))
    import resolve  # noqa: E402
    gen = os.path.join(os.environ.get('RDR_MAIN_CHECKOUT') or resolve.main_checkout(), 'generated', 'default')
    if not os.path.isdir(gen):
        raise RuntimeError('no generated code at %s' % gen)
    fns = {n: find_function(gen, n) for n in ('sub_82866680', 'sub_82833440', 'sub_82AA3CD0', 'sub_824F6130', 'sub_826C7AA0', 'sub_8276D338',
                                              'sub_827A9BC8', 'sub_82A9CF30', 'sub_82A9D490', 'sub_82A9F208', 'sub_8284E1E0', 'sub_82A9D9F8', 'sub_828520D8', 'sub_82AA54F0',
                                              'sub_828265C0', 'sub_82A9D2F8', 'sub_827DCF40', 'sub_82826638')}

    def rx(name, pattern):
        return re.search(pattern, fns[name]) is not None
    pins = [
        ('sub_82866680 (screen 1501): S created, progress load, then exit action 3 (sub_8283CFF0, r4 = 3)',
         rx('sub_82866680', r'sub_828265C0\(ctx, base\);.*sub_82A9D750\(ctx, base\); ctx\.r4\.s64 = 3; ctx\.r3\.u64 = REX_LOAD_U32\(ctx\.r30\.u32 \+ -27556\); ctx\.lr = 0x8286672C; sub_8283CFF0\(ctx, base\);')),
        ('sub_82866680 (screen 1500): exit action 2 = the 30 Years menus (the path this lane skips)',
         rx('sub_82866680', r'ctx\.r4\.s64 = 2; ctx\.r3\.u64 = REX_LOAD_U32\(ctx\.r11\.u32 \+ -27556\); ctx\.lr = 0x82866744; sub_8283CFF0\(ctx, base\);')),
        ('sub_82833440: case 3 = heap 0 bracket then bl sub_82AA3CD0 at 0x82833600 (after sub_82B04C10(.., 2))',
         rx('sub_82833440', r'ctx\.r4\.s64 = 2; ctx\.lr = 0x828335EC; sub_82B04C10\(ctx, base\); .*ctx\.lr = 0x828335F0; sub_82C2D8C0\(ctx, base\); ctx\.r31\.u64 = ctx\.r3\.u64; ctx\.r3\.s64 = 0; ctx\.lr = 0x828335FC; sub_82C2DE28\(ctx, base\); ctx\.lr = 0x82833600; sub_82AA3CD0\(ctx, base\);')),
        ('sub_82AA3CD0: switch_sequence(obj, 3, 0x82519858) (r11 = 0x82520000, r5 = r11 - 26536)',
         rx('sub_82AA3CD0', r'ctx\.r11\.s64 = -2108555264; ctx\.r4\.s64 = 3; ctx\.r5\.u64 = ctx\.r11\.u64 \+ uint64_t\(int64_t\(-26536\)\); ctx\.lr = 0x82AA3D5C; sub_8225E790\(ctx, base\);')),
        ('sub_824F6130: flow maker 0x8201B7A0 (0x82020000 - 18528) behind the sub_827D75A0 test and the byte G+466 test',
         rx('sub_824F6130', r'sub_827D75A0\(ctx, base\);.*sub_827D7578\(ctx, base\); ctx\.r11\.u64 = REX_LOAD_U8\(ctx\.r3\.u32 \+ 466\);.*ctx\.r10\.u64 = ctx\.r11\.u64 \+ uint64_t\(int64_t\(-18528\)\)')),
        ('sub_826C7AA0: SetState(29) through the flow\'s vtable slot 1',
         rx('sub_826C7AA0', r'ctx\.r4\.s64 = 29; ctx\.r11\.u64 = REX_LOAD_U32\(ctx\.r3\.u32 \+ 0\); ctx\.r10\.u64 = REX_LOAD_U32\(ctx\.r11\.u32 \+ 4\); ctr\.u64 = ctx\.r10\.u64;')),
        ('sub_8276D338: state 29 -> sub_827A9BC8 (and 30 -> sub_827AA458)',
         rx('sub_8276D338', r'compare<int32_t>\(ctx\.r4\.s32, 29, xer\); if \(cr6\.eq\) goto (loc_\w+);') and
         rx('sub_8276D338', r'ctx\.r3\.s64 = 28; ctx\.lr = 0x8276D388; sub_82250050\(ctx, base\); .*sub_827A9BC8\(ctx, base\);') and 'sub_827AA458(ctx, base);' in fns['sub_8276D338']),
        ('sub_827A9BC8: state id 29, vtable 0x82032414 (0x82030000 + 9236)',
         rx('sub_827A9BC8', r'ctx\.r10\.s64 = -2113732608; .*ctx\.r9\.s64 = 29; ctx\.r8\.u64 = ctx\.r10\.u64 \+ uint64_t\(int64_t\(9236\)\);')),
        ('sub_82A9CF30: S ctor saves G+18576 in S+40 and sets it to 2',
         rx('sub_82A9CF30', r'ctx\.r9\.u64 = REX_LOAD_U32\(ctx\.r3\.u32 \+ 18576\); REX_STORE_U32\(ctx\.r31\.u32 \+ 40, ctx\.r9\.u32\);.*ctx\.r8\.s64 = 2; REX_STORE_U32\(ctx\.r3\.u32 \+ 18576, ctx\.r8\.u32\);')),
        ('sub_82A9D490: S dtor restores G+18576 from S+40',
         rx('sub_82A9D490', r'ctx\.r11\.u64 = REX_LOAD_U32\(ctx\.r31\.u32 \+ 40\); REX_STORE_U32\(ctx\.r3\.u32 \+ 18576, ctx\.r11\.u32\);')),
        ('sub_82A9D9F8: (B, era, pad): B+4 = pad, B+2348 = era, B+2352 = 10000 (so the code is written AFTER it)',
         rx('sub_82A9D9F8', r'ctx\.r31\.u64 = ctx\.r3\.u64; ctx\.r30\.u64 = ctx\.r4\.u64; ctx\.r29\.u64 = ctx\.r5\.u64;') and
         rx('sub_82A9D9F8', r'REX_STORE_U32\(ctx\.r31\.u32 \+ 4, ctx\.r29\.u32\);') and
         rx('sub_82A9D9F8', r'ctx\.r5\.s64 = 10000;.*REX_STORE_U32\(ctx\.r31\.u32 \+ 2348, ctx\.r30\.u32\);.*REX_STORE_U32\(ctx\.r31\.u32 \+ 2352, ctx\.r5\.u32\);')),
        ('sub_828520D8 (the list\'s pick): sub_82A9D9F8, then B+2352 = code, then var 215',
         rx('sub_828520D8', r'sub_82A9D9F8\(ctx, base\); REX_STORE_U32\(ctx\.r30\.u32 \+ 2352, ctx\.r28\.u32\);.*ctx\.r3\.s64 = 215;.*sub_82A9CAD8\(ctx, base\);')),
        ('sub_82AA54F0 (native 8): sets var 215 itself (after sub_82A9F1E0)',
         rx('sub_82AA54F0', r'sub_82A9F1E0\(ctx, base\);.*ctx\.r3\.s64 = 215;.*sub_82A9CAD8\(ctx, base\);')),
        ('sub_82A9F208 (the picture table setter): [A + 4*(slot+45)] = id for slot < 80',
         rx('sub_82A9F208', r'\+ 45;') or rx('sub_82A9F208', r'int64_t\(45\)') or rx('sub_82A9F208', r'\b45\b')),
        ('sub_8284E1E0 (the list build) calls sub_82A9DB50, sub_82A9DB90 and sub_82A9F208',
         all(('%s(ctx, base);' % n) in fns['sub_8284E1E0'] for n in ('sub_82A9DB50', 'sub_82A9DB90', 'sub_82A9F208'))),
        ('sub_82A9D2F8 / sub_828265C0 / sub_827DCF40 / sub_82826638 exist as functions the build has (the lane calls / mirrors them)',
         all(len(fns[n]) > 40 for n in ('sub_82A9D2F8', 'sub_828265C0', 'sub_827DCF40', 'sub_82826638'))),
        ('sub_82826638 (sequence 31 ctor): calls sub_82338B68 and sub_82338AF0 (the pre-creates) and sub_828265C0 or the S create',
         'sub_82338B68(ctx, base);' in fns['sub_82826638'] and 'sub_82338AF0(ctx, base);' in fns['sub_82826638']),
    ]
    for what, ok in pins:
        check(ok, 'listing pin failed: %s' % what)
    print('  listing pins: %d checked' % len(pins))


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else ''
    try:
        if mode == '--listing':
            listing_main()
        else:
            order_main()
            wiring_main()
    except (RuntimeError, ValueError, OSError) as x:
        check(False, 'cannot read an input: %s' % x)
    if problems:
        print('WMB selftest: %d problem(s)' % len(problems))
        sys.exit(1)
    print('WMB selftest: ok')


if __name__ == '__main__':
    main()
