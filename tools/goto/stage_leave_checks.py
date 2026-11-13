"""S101 lane 72 (go anywhere's two gaps: the set behind a pushed scene, and leaving the stage): the offline pins, called from stage_selftest.py (gate lane scene_stage). No game run.

Design: docs/research/s101/GOANY_stage_set_and_leave.md. What is checked, all against the shipped files:
 8. Arena: src/scene_stage_logic.h ArenaLoadable (clang++ via scene_stage_test.cpp) answers every id 0-300 like the player's own pac\\bg\\bgNN.pac listing (read only, counts
    only) and like the design's list; the python mirror (address.LOADABLE_ARENAS) is the same list; scene:ID@arena:N addresses plan the arena into the play step; planted
    list faults (off by one at each edge, the 76-80 branch dropped, created arenas allowed) must be caught.
 9. Leave guard: LeaveRefusal truth table (OK for the run 7072-like words, refused for each missing condition); planted: the P+8 guard dropped must fail on the run 7066-class
    state, the M+20 guard dropped must fail.
10. Text pins on src/scene_director.h: FirePlay calls ArenaLoadable after HoldsInStage and before ReadStageWords; FireLeave = LeaveRefusal, then Q+40 := 3, then sub_82A3E130 and
    nothing else; the director never stores to H / L / the story root pointer; LeaveTick runs before StageTick; the per-frame stage line carries G+64 / SL+36 / CS; the
    `play: dispatched` line carries no class verdict (it is its own line).
11. Past facts re-found from log fixtures (log lines only): coverage run 012 (the editor reloads the set inside sequence 9: BG76 -> BG01 -> BG78, one INGAMELUA open);
    run 7072 (our push asked for the loaded arena 76: one BG76 open, none after either dispatch, the command vtable read 0 on the dispatched line); run 7066 (sequence 9 -> 75
    with the listener still registered: BackAtMenuProblem flags it; planted: ignoring the listener misses it).
12. Listing pins (the generated code in the main checkout, read only): the instructions the design's arguments rest on, with planted-text twins.
13. The parse rule knows `leave` (and refuses arguments).
"""
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))

DESIGN_LOADABLE = list(range(0, 10)) + list(range(11, 18)) + list(range(21, 24)) + list(range(28, 57)) + list(range(66, 69)) + list(range(76, 81))
LOAD_EDGES = (0, 1, 4, 9, 11, 17, 21, 23, 28, 56, 66, 68, 76, 78, 80)
REFUSE_EDGES = (10, 18, 20, 24, 27, 57, 65, 69, 75, 81, 199, 200, 214)

# a leave-ready stage as run 7072 left it (words are the design's; P+4 1 = Q running, P+8 0 = P built M, Q+16 2, Q+28 2, M+20 2)
LEAVE_FIELDS = ['seq', 'r', 'g_mode', 'p', 'p_vtable', 'p_state', 'p_owns', 'q', 'q_vtable', 'q_life', 'q_mode', 'q_slot7', 'm', 'm_state', 'pending', 'ours', 'q_child']
LEAVE_OK = dict(seq=9, r=0xA4B69FC0, g_mode=4, p=0xA4B79820, p_vtable=0x82057BD0, p_state=1, p_owns=0, q=0xA4B7A000, q_vtable=0x820585B0, q_life=2, q_mode=2,
                q_slot7=0x82A3E130, m=0xA646BB80, m_state=2, pending=0, ours=0, q_child=0)
LEAVE_BAD = [  # (what, overrides) each must be refused
    ('not in game', dict(seq=75)), ('R null', dict(r=0)), ('not story mode', dict(g_mode=3)), ('no program', dict(p=0)), ('wrong program class', dict(p_vtable=0x8200A410)),
    ('P+4 0 busy', dict(p_state=0)), ('P+4 2 leaving', dict(p_state=2)), ('P+4 3 leaving', dict(p_state=3)), ('P+4 9 unknown', dict(p_state=9)), ('Q null', dict(q=0)),
    ('wrong Q class', dict(q_vtable=0x82057BD0)), ('Q+16 3', dict(q_life=3)), ('Q+16 5', dict(q_life=5)), ('Q+28 0 (editor UI)', dict(q_mode=0)), ('M null', dict(m=0)),
    ('M+20 3', dict(m_state=3)), ('M+20 4', dict(m_state=4)), ('P+8 1 (root pre-dates the stage)', dict(p_owns=1)), ('push pending', dict(pending=1)),
    ('our push not committed', dict(ours=1)), ('Q+36 child set', dict(q_child=0x40001000)), ('slot 7 is not the cancel', dict(q_slot7=0x82A3E0D8)),
]
# the listing pins: (function, address, text the instruction must contain); each is a word the design's argument rests on
PINS = [
    ('8232BD60', 0x8232BDC4, 'lwz r11,12(r29)'), ('8232BD60', 0x8232BDD4, 'bl 0x822ff368'), ('8232BD60', 0x8232BDE0, 'stw r28,64(r3)'),   # G+64 := ctx(read)+16 (type 4)
    ('823379C8', 0x82337A00, 'lwz r11,36(r11)'), ('823379C8', 0x82337A04, 'cmpw cr6,r11,r10'),                                           # reload when SL+36 != G+64
    ('82578238', 0x825783D4, 'bl 0x827e4090'),                                                                                          # the stage load request
    ('82A19A18', 0x82A19A30, 'li r29,76'),                                                                                              # the default arena is 76
    ('82A3E130', 0x82A3E170, 'bl 0x823be7d0'),                                                                                          # Q slot 7 -> Q+16 = 4
    ('82A3EBA8', 0x82A3ECAC, 'li r11,3'), ('82A3EBA8', 0x82A3ECB8, 'stw r11,40(r31)'), ('82A3EBA8', 0x82A3ECBC, 'lwz r9,28(r10)'),        # the editor's cancel: Q+40 = 3, slot 7
    ('82A19810', 0x82A1986C, 'lwz r11,8(r31)'), ('82A19810', 0x82A19868, 'bne cr6,0x82a19878'), ('82A19810', 0x82A19878, 'bl 0x82a28ba8'),  # shutdown unless Q+20 == 0 and P+8 != 0
    ('82A28BA8', 0x82A28BF0, 'bl 0x829088d0'), ('82A28BA8', 0x82A28BF8, 'stw r10,20(r31)'),                                               # M shutdown stops H, M+20 = 3
    ('82A19D58', 0x82A19F4C, 'stw r31,88(r3)'),                                                                                         # PG+88 from the exit code
    ('82833440', 0x82833544, 'stw r11,-30068(r10)'),                                                                                    # the dispatcher stores 1 at 0x83628A8C
    ('827DDB28', 0x827DDF68, 'cmpwi cr6,r11,2'), ('827DDB28', 0x827DDFA0, 'bl 0x8225e790'),                                              # PG+88 == 2 -> the SmartMenu sequence
    ('823BE810', 0x823BE814, 'addi r11,r11,-5'),                                                                                        # "finished" = Q+16 == 5
]


def listing(main_checkout, fn):
    """-> {address: text} for the recompiled function fn (tools/analysis/list_function.py, run in the main checkout where generated\\ lives). RuntimeError when unreadable."""
    r = subprocess.run([sys.executable, '-I', os.path.join(os.path.dirname(os.path.dirname(HERE)), 'tools', 'analysis', 'list_function.py'), fn], capture_output=True, text=True,
                       cwd=main_checkout, timeout=120)
    out = {}
    for ln in r.stdout.splitlines():
        m = re.match(r'0x([0-9A-F]{8})  (.*)', ln)
        if m:
            out[int(m.group(1), 16)] = m.group(2).strip()
    if not out:
        raise RuntimeError('no listing for sub_%s (the generated code is missing or unreadable in %s): %s' % (fn, main_checkout, (r.stdout + r.stderr)[:200]))
    return out


def pin_problems(listings, pins):
    p = []
    for fn, addr, text in pins:
        got = listings[fn].get(addr)
        if got is None or text not in got:
            p.append('sub_%s 0x%08X: want %r, the listing has %r' % (fn, addr, text, got))
    return p


def run_exe(exe, lines):
    return subprocess.run([exe], input=''.join(ln + '\n' for ln in lines), capture_output=True, text=True, timeout=60).stdout.split('\n')[:len(lines)]


def leave_line(**kw):
    v = dict(LEAVE_OK)
    v.update(kw)
    return 'leave ' + ' '.join(str(v[n]) for n in LEAVE_FIELDS)


def arena_problems(exe):
    p = []
    ids = list(range(0, 301))
    got = run_exe(exe, ['arena %d' % n for n in ids])
    for n, g in zip(ids, got):
        want = 'LOAD' if n in DESIGN_LOADABLE else 'REFUSE'
        if g.split(' ')[0] != want:
            p.append('arena %d: want %s, got %s' % (n, want, g[:40]))
    named = dict(zip(ids, got))
    for n in LOAD_EDGES:
        if named[n] != 'LOAD':
            p.append('edge %d must load' % n)
    for n in REFUSE_EDGES:
        if not named[n].startswith('REFUSE'):
            p.append('edge %d must be refused' % n)
    if '(200+)' not in named[200] or 'created' not in named[214]:
        p.append('a created arena (200+) must say so: %r' % named[200][:80])
    if 'no BGnn.PAC' not in named[10]:
        p.append('arena 10 must say it has no BGnn.PAC: %r' % named[10][:80])
    notes = run_exe(exe, ['arenanote 76 0', 'arenanote 76 1', 'arenanote 4 1', 'arenanote 4 0'])
    if "arena 76 = the editor's empty stage" not in notes[0] or 'pass arena=N' not in notes[0]:
        p.append('no arena given must say "arena 76 = the editor\'s empty stage ... pass arena=N": %r' % notes[0])
    if 'pass arena=N' in notes[1] or "empty stage" not in notes[1]:
        p.append('arena 76 given must still say it is the empty stage, without the hint: %r' % notes[1])
    if 'arena 4' not in notes[2]:
        p.append('arena 4 note: %r' % notes[2])
    return p


def leave_problems(exe):
    p = []
    lines = [leave_line()] + [leave_line(**o) for _, o in LEAVE_BAD]
    got = run_exe(exe, lines)
    if got[0] != 'OK':
        p.append('the run 7072-like words must be OK, got %r' % got[0][:90])
    for (what, _), g in zip(LEAVE_BAD, got[1:]):
        if not g.startswith('NOT '):
            p.append('leave must be refused for %s, got %r' % (what, g[:60]))
    return p


def back_problems(exe, stale_listener):
    p = []
    got = run_exe(exe, ['back 0 0 0 0', 'back %d 0 0 0' % stale_listener, 'back 0 4 0 0', 'back 0 0 4 0', 'back 0 0 0 4'])
    if got[0] != 'CLEAN':
        p.append('all-zero words must be CLEAN: %r' % got[0])
    for what, g in zip(('a stale listener', 'a stale H', 'a stale L', 'a stale M'), got[1:]):
        if not g.startswith('LEFT'):
            p.append('%s at the main menu must be flagged: %r' % (what, g))
    return p


def consts_problems(exe):
    want = {'kStageLoadMgrPtr': 0x83924738, 'kSLArena': 36, 'kLoadedStagePtr': 0x83763544, 'kGArena': 64, 'kStoryProgramVtable': 0x82057BD0, 'kStoryObjectVtable': 0x820585B0,
            'kQSlot7Word': 0x820585CC, 'kQCancelFunction': 0x82A3E130, 'kProgState': 4, 'kProgOwnsRoot': 8, 'kProgQ': 12, 'kQLife': 16, 'kQMode': 28, 'kQExit': 40, 'kQExitCancel': 3,
            'kRootState': 20, 'kLoaderH': 0x839294B8, 'kLoaderL': 0x839294B4, 'kIngameProgramPtr': 0x8376AAF0, 'kPGNext': 88}  # the design page (and q.py: 0x820585CC points at sub_82A3E130)
    out = subprocess.run([exe], input='consts\n', capture_output=True, text=True, timeout=60).stdout
    have = {}
    for ln in out.split('\n'):
        if '=' in ln:
            k, v = ln.split('=', 1)
            have[k] = int(v)
    return ['constant %s is %s, the design says %s' % (k, have.get(k), v) for k, v in want.items() if have.get(k) != v]


def static_problems(src, args_h, ss_mod):
    p = []
    body = lambda sig: ss_mod.body_of(src, sig)  # noqa: E731
    play, leave, tick, ltick = body('inline std::string FirePlay('), body('inline std::string FireLeave('), body('inline void StageTick('), body('inline void LeaveTick(')
    main_tick, live_cmd = body('inline void Tick('), body('inline std::string LiveCommand(')
    for name, b in (('FirePlay', play), ('FireLeave', leave), ('StageTick', tick), ('LeaveTick', ltick)):
        if not b:
            p.append('%s not found' % name)
    if not (play and leave and tick and ltick):
        return p
    # arena: refused after the bank test and before any guest read
    a, h, r = play.find('ArenaLoadable(uint32_t(a.arena))'), play.find('HoldsInStage(uint32_t(id))'), play.find('ReadStageWords()')
    if not (0 <= h < a < r):
        p.append('FirePlay must call ArenaLoadable( after HoldsInStage( and before ReadStageWords() (%d %d %d)' % (h, a, r))
    if 'ArenaRefusal(' not in play or 'ArenaNote(' not in play:
        p.append('FirePlay must answer with the ArenaRefusal / ArenaNote texts')
    if re.search(r'call\((sub_[0-9A-F]{8})', play) is None or re.findall(r'call\((sub_[0-9A-F]{8})', play) != ['sub_82A22580', 'sub_8231A5D0', 'sub_82A30F08']:
        p.append('FirePlay must keep the handler recipe calls only')
    if 'if (g_leave_on) return "refused: a leave is running"' not in play or play.find('g_leave_on') > play.find('BankExists'):
        p.append('FirePlay must refuse while a leave is running, before anything else')
    if 'g_stage_on = false;' not in ltick[ltick.find('did not complete'):]:
        p.append('the leave timeout must clear g_stage_on')
    if 'LeaveRd(listener, ss::kProgQ) == g_leave_q' not in ltick:
        p.append('the watcher must read Q only while P still holds it')
    # leave: guard, then Q+40 := 3, then the one call
    g, w, c = leave.find('LeaveRefusal(w)'), leave.find('StagePut32(w.q + ss::kQExit, ss::kQExitCancel)'), leave.find('call(sub_82A3E130, {w.q})')
    if not (0 <= g < w < c):
        p.append('FireLeave must run LeaveRefusal, then store Q+40 = 3, then call sub_82A3E130 (%d %d %d)' % (g, w, c))
    if re.findall(r'call\((sub_[0-9A-F]{8})', leave) != ['sub_82A3E130'] or set(ss_mod.FUNC_RE.findall(leave)) != {'sub_82A3E130'}:
        p.append('FireLeave may call sub_82A3E130 and nothing else (no shutdown, no deletes): %s' % sorted(set(ss_mod.FUNC_RE.findall(leave))))
    if len(re.findall(r'StagePut32\(', leave)) != 1:
        p.append('FireLeave writes exactly one guest word (Q+40): %d StagePut32 calls' % len(re.findall(r'StagePut32\(', leave)))
    if 'SetPlaying(false)' not in leave:
        p.append('FireLeave must clear the stage view playing flag')
    if 'refused: " + why' not in leave:
        p.append('FireLeave must refuse with the guard reason')
    # the rejected fix: nothing in the director stores to H / L / M (the story root pointer), anywhere
    for m in re.finditer(r'StagePut32\(([^,]*),', src):
        arg = m.group(1)
        if re.search(r'kLoader[HL]|0x839294B[48]|kStoryRootPtr|0x839295CC|kStageLoadMgrPtr|kLoadedStagePtr', arg):
            p.append('the director stores to a game global it must never write: StagePut32(%s' % arg)
    if re.search(r'store_and_swap[^;]*(kLoader|0x839294B|kStoryRootPtr|0x839295CC)', src):
        p.append('the director stores to H / L / M through store_and_swap')
    # served on the game thread by the owner tick, before StageTick drops the stage
    if 'kind == 7 ? FireLeave(ctx, base, a)' not in src:
        p.append('Tick must serve kind 7 (leave)')
    if not re.search(r'case scene::Verb::kPlay:\s*case scene::Verb::kLeave: \{', live_cmd) or 'kLeave ? 7' not in live_cmd:
        p.append('LiveCommand must queue leave as kind 7 like the other verbs')
    if re.search(r'U32\(|Guest\(|ReadLeaveWords', live_cmd):
        p.append('LiveCommand reads guest memory on the pipe thread')
    if 'LeaveTick();' not in main_tick or 'StageTick();' not in main_tick or main_tick.find('LeaveTick();') > main_tick.find('StageTick();'):
        p.append('Tick must call LeaveTick before StageTick')
    # the watcher: reads, never writes; ends at sequence 75 with the last sequence-9 words; times out once, no second attempt
    if 'StagePut32' in ltick or 'store_and_swap' in ltick or 'call(' in ltick:
        p.append('LeaveTick must only read')
    for need in ('BackAtMenuProblem(', 'kLeaveTimeoutFrames', 'back at the main menu', 'did not complete', 'kPGNext', 'kLoaderH', 'kLoaderL', 'kProgState', 'kRootState', 'kQLife'):
        if need not in ltick:
            p.append('LeaveTick lost %s' % need)
    # the per-frame stage line, the dispatched line
    if 'G+64={} SL+36={} CS={:08X}' not in tick or 'kGArena' not in tick or 'kSLArena' not in tick or 'kLoadedStagePtr' not in tick:
        p.append('the per-frame stage line must carry G+64, SL+36 and the stage object word')
    m = re.search(r'REXLOG_INFO\("\[wwe2k14\]\[scene\]\[director\] (play: dispatched scene [^"]*)"', src)
    if not m or 'vtable' in m.group(1) or 'command class' in m.group(1):
        p.append('the `play: dispatched` line must not read the command class (the command object is not built yet on that frame)')
    if 'play: command class of scene ' not in tick or 'w.cmd_vtable != 0' not in tick:
        p.append('the command class is logged on its own line, on the first non-zero vtable')
    for need in ('REX_EXTERN(sub_82A3E130);', 'leave: requested'):
        if need not in src:
            p.append('missing: %s' % need)
    return p


def planted_twins_static(src):
    return {
        'arena test removed': src.replace('  if (a.arena >= 0 && !ss::ArenaLoadable(uint32_t(a.arena))) return ss::ArenaRefusal(uint32_t(a.arena));', '  (void)0;', 1),
        'arena test after the ready read': src.replace('  if (a.arena >= 0 && !ss::ArenaLoadable(uint32_t(a.arena))) return ss::ArenaRefusal(uint32_t(a.arena));  // S101 lane 72: before any guest read or write\n', '', 1).replace(
            '  const std::string why = ss::NotReady(w, g_held_bank);', '  if (a.arena >= 0 && !ss::ArenaLoadable(uint32_t(a.arena))) return "x";\n  const std::string why = ss::NotReady(w, g_held_bank);', 1),
        'play while a leave runs': src.replace('  if (g_leave_on) return "refused: a leave is running";  // S101 lane 72 review F1\n', '', 1),
        'leave timeout keeps the stage': src.replace('    g_stage_on = false;  // S101 lane 72 review F1: the stage is over or unknown; a new `!scene stage` starts clean\n', '', 1),
        'watcher reads Q after P let go': src.replace('LeaveRd(listener, ss::kProgQ) == g_leave_q ? LeaveRd(g_leave_q, ss::kQLife) : 0', 'LeaveRd(g_leave_q, ss::kQLife)', 1),
        'leave without the guard': src.replace('  const std::string why = ss::LeaveRefusal(w);\n  if (!why.empty()) return "refused: " + why;\n', '', 1),
        'leave writes the exit code before the guard': src.replace('  const ss::LeaveWords w = ReadLeaveWords();\n', '  const ss::LeaveWords w = ReadLeaveWords();\n  StagePut32(w.q + ss::kQExit, ss::kQExitCancel);\n', 1),
        'leave clears H too': src.replace('  call(sub_82A3E130, {w.q});\n', '  call(sub_82A3E130, {w.q});\n  StagePut32(ss::kLoaderH, 0);\n', 1),
        'leave clears the story root pointer': src.replace('  call(sub_82A3E130, {w.q});\n', '  call(sub_82A3E130, {w.q});\n  StagePut32(0x839295CC, 0);\n', 1),
        'leave also calls the root shutdown': src.replace('  call(sub_82A3E130, {w.q});\n', '  call(sub_82A3E130, {w.q});\n  call(sub_82A28BA8, {0});\n', 1),
        'leave watcher after the stage tick': src.replace('  LeaveTick();  // S101 lane 72: before StageTick, which drops the stage at sequence 75\n  StageTick();  // S101 lane 38\n',
                                                          '  StageTick();  // S101 lane 38\n  LeaveTick();\n', 1),
        'leave watcher writes': src.replace('  if (!g_leave_on) return;\n  const uint64_t f = Frame();', '  if (!g_leave_on) return;\n  StagePut32(0x839294B8, 0);\n  const uint64_t f = Frame();', 1),
        'dispatched line reads the class again': src.replace('G+18572(mode)={}, G+64={}, frame={}",', 'G+18572(mode)={}, command class vtable, G+64={}, frame={}",', 1),
        'stage line without G+64': src.replace(' G+64={} SL+36={} CS={:08X} seq={} phase={}"', ' seq={} phase={}"', 1),
        'leave not served': src.replace('kind == 7 ? FireLeave(ctx, base, a)', 'kind == 7 ? std::string("armed")', 1),
        'leave reads guest on the pipe thread': src.replace('      g_request = a;\n', '      g_request = a; (void)ReadLeaveWords();\n', 1),
    }


def leave_sequence_problems(lines):
    """The word sequence the run 7073 leave showed (Check E3): Q+16 4 -> 5; P+4 1 -> 2 -> 3 -> 0; M+20 2 -> 3 -> 0; L -> 0 before H / M; H, M, listener -> 0 together; PG+88 2;
    sequence 9 -> 75; the director's own 'orderly end' line. Reads log lines only."""
    rows = []
    for ln in lines:
        m = re.search(r'leave \+(\d+) Q\+16=(\d+) P\+4=(\d+) M\+20=(\d+) M\+24=(\d+) H=([0-9A-F]{8}) L=([0-9A-F]{8}) M=([0-9A-F]{8}) listener=([0-9A-F]{8}) R\+9640=\d+ PG\+88=(\d+) G\+18576=(\d+) seq=(\d+)', ln)
        if m:
            rows.append(dict(zip(('at', 'q', 'p', 'm20', 'm24', 'h', 'l', 'm', 'lis', 'pg', 'g', 'seq'), [int(m.group(1))] + [int(x) for x in m.groups()[1:5]] + list(m.groups()[5:9]) + [int(x) for x in m.groups()[9:]])))
    p = []
    if len(rows) < 8:
        return ['only %d leave rows' % len(rows)]

    def firsts(key):
        out = []
        for r in rows:
            if not out or out[-1] != r[key]:
                out.append(r[key])
        return out
    if firsts('q') != [4, 5, 0]: p.append('Q+16 sequence %s' % firsts('q'))
    if firsts('p') != [1, 2, 3, 0]: p.append('P+4 sequence %s' % firsts('p'))
    if firsts('m20') != [2, 3, 0]: p.append('M+20 sequence %s' % firsts('m20'))
    if firsts('seq') != [9, 75]: p.append('sequence %s' % firsts('seq'))
    if {r['pg'] for r in rows} != {2}: p.append('PG+88 values %s' % {r['pg'] for r in rows})
    l0 = next((i for i, r in enumerate(rows) if r['l'] == '00000000'), None)
    z = next((i for i, r in enumerate(rows) if r['h'] == '00000000'), None)
    if l0 is None or z is None or not l0 < z: p.append('L must reach 0 before H (L at row %s, H at row %s)' % (l0, z))
    elif not (rows[z]['m'] == '00000000' and rows[z]['lis'] == '00000000' and rows[z - 1]['h'] != '00000000' and rows[z - 1]['m'] != '00000000'):
        p.append('H, M and the listener must reach 0 on the same row')
    if not any('leave: back at the main menu (the last sequence-9 listener/H/L/M were all 0: the orderly end)' in ln for ln in lines): p.append('no orderly-end line')
    return p


def run(check, rd, build_logic, resolve, address, stage_log, ss_mod, scene_selftest_module, fixtures_dir):
    # ---- 8. arena ----
    header = rd('src', 'scene_stage_logic.h')
    with tempfile.TemporaryDirectory() as tmp:
        exe = build_logic(header, tmp, 'lane72_real')
        bad = arena_problems(exe) + leave_problems(exe) + consts_problems(exe)
        check(not bad, 'lane 72 shipped logic (arena / leave): %s' % bad[:4])
        # the python mirror and the player's files
        check(list(address.LOADABLE_ARENAS) == DESIGN_LOADABLE, 'address.LOADABLE_ARENAS differs from the design list')
        game_dir = os.path.join(resolve.main_checkout(), 'game_data', 'extracted', 'PAC', 'BG')
        files = None
        if os.path.isdir(game_dir):
            files = sorted(int(m.group(1)) for m in (re.fullmatch(r'bg(\d\d)\.pac', f, re.I) for f in os.listdir(game_dir)) if m)
            check(files == DESIGN_LOADABLE, 'the player\'s PAC\\BG listing (%d files bgNN.pac) differs from the loadable list (%d ids): extra %s, missing %s' % (
                len(files), len(DESIGN_LOADABLE), sorted(set(files) - set(DESIGN_LOADABLE)), sorted(set(DESIGN_LOADABLE) - set(files))))
        else:
            check(False, 'cannot read the player\'s PAC\\BG folder %s (the loadable list is verified against it)' % game_dir)
        # addresses
        a = address.parse('scene:4001:324/1,139@arena:4')
        check(a.arena == 4 and a.scene_ids == (4001,) and a.performers == ((324, 1), (139, -1)) and str(a) == 'scene:4001:324/1,139@arena:4', 'scene@arena parse: %s' % (a,))
        check(address.parse('scene:4001@arena:78').arena == 78 and address.parse('scene:4001').arena == -1, 'scene@arena without performers / without arena')
        for t in ('scene:4001@arena:10', 'scene:4001@arena:200', 'scene:4001@arena:81', 'scene:4001@arena:', 'scene:4001@arena:x', 'scene:4001@arena:4@arena:5', 'scene:4001@arena:1234',
                  'scene:4001@arena:-1', 'scene:leave:324', 'scene:leaves'):
            try:
                address.parse(t)
                check(False, 'malformed address accepted: %r' % t)
            except address.AddressError as x:
                check(bool(str(x)), 'refusal without a reason: %r' % t)
        check(address.parse('scene:leave').leave and str(address.parse('scene:leave')) == 'scene:leave', 'scene:leave parse')
        ctx = resolve.Context()
        pl = resolve.resolve('scene:4001:324/1,139@arena:4', ctx)
        check(pl.status == 'plan' and pl.steps[3]['op'] == 'scene_play' and pl.steps[3]['arena'] == 4 and resolve.describe_step(pl.steps[3]).endswith('!scene play 4001 324/1 139 arena=4'),
              'the plan must carry the arena into the play step: %s' % (pl.steps[3:4],))
        pl0 = resolve.resolve('scene:4001:324', ctx)
        check(pl0.steps[3]['arena'] == 1 and any("the show's arena" in n for n in pl0.notes), 'S101 lane 77: no arena given on a show-arena scene: RAW (1) with the show note')
        check(resolve.scene_arg('play', [4001], performers=[(324, 1)], arena=4) == 'play 4001 324/1 arena=4', 'scene_arg play arena')
        lv = resolve.resolve('scene:leave', ctx)
        check(lv.status == 'plan' and [s['op'] for s in lv.steps] == ['scene_leave', 'expect_scene_log'] and lv.steps[1]['text'] == 'leave: back at the main menu' and resolve.scene_arg('leave', []) == 'leave',
              'scene:leave must plan scene_leave then wait for the back-at-the-menu line: %s' % (lv.steps,))
        # planted list faults
        twins = {
            'the 76-80 branch dropped': header.replace(' || (n >= 76 && n <= 80)', ''),
            '28-56 widened to 57': header.replace('(n >= 28 && n <= 56)', '(n >= 28 && n <= 57)'),
            '28-56 starts at 27': header.replace('(n >= 28 && n <= 56)', '(n >= 27 && n <= 56)'),
            '0-9 widened to 10': header.replace('n <= 9 ||', 'n <= 10 ||'),
            '11-17 starts at 10': header.replace('(n >= 11 && n <= 17)', '(n >= 10 && n <= 17)'),
            '21-23 ends at 24': header.replace('(n >= 21 && n <= 23)', '(n >= 21 && n <= 24)'),
            '66-68 dropped': header.replace(' || (n >= 66 && n <= 68)', ''),
            '76-80 ends at 81': header.replace('(n >= 76 && n <= 80)', '(n >= 76 && n <= 81)'),
            'created arenas allowed': header.replace(' || (n >= 76 && n <= 80);', ' || (n >= 76 && n <= 80) || (n >= 200 && n <= 214);'),
            'the P+8 guard dropped': header.replace('  if (w.p_owns != 0) {', '  if (false) {'),
            'the Q+36 guard dropped': header.replace('  if (w.q_child != 0) return', '  if (false) return'),
            'the M+20 guard dropped': header.replace('  if (w.m_state != 2) return', '  if (false) return'),
            'the P+4 guards dropped': header.replace('  if (w.p_state == 2 || w.p_state == 3) return', '  if (false) return').replace('  if (w.p_state != 1) return', '  if (false) return'),
            'the slot 7 pin dropped': header.replace('  if (w.q_slot7 != kQCancelFunction) return', '  if (false) return'),
            'a pending push ignored': header.replace('  if (w.pending != 0 || w.play_pending_ours) return', '  if (false) return'),
            'the Q+28 mode guard dropped': header.replace('  if (w.q_mode != 2) return', '  if (false) return'),
            'the listener ignored at the menu': header.replace('  if (listener) s += " listener="', '  if (false) s += " listener="'),
            'the arena 76 note lost': header.replace('if (arena == kEmptyStageArena) {', 'if (false) {'),
        }
        # the 7066-class state for the P+8 twin: the words the run 7066 stage had, as far as the log shows them (R live, listener A4B79820, G+18576 4, seq 9), with P+8 modelled as 1
        for what, text in twins.items():
            if text == header:
                check(False, 'planted twin "%s" did not change the header' % what)
                continue
            try:
                texe = build_logic(text, tmp, 'lane72_' + re.sub(r'\W', '', what))
                got = arena_problems(texe) + leave_problems(texe) + back_problems(texe, 0xA4B79820)
            except RuntimeError:
                got = ['does not compile']
            check(bool(got), 'planted lane 72 logic fault (%s) was not caught' % what)
        # ---- 11. the run 7066 states ----
        lines7066 = open(os.path.join(fixtures_dir, 'stage_run7066_lines.txt'), encoding='utf-8').read().split('\n')
        states = [ln for ln in lines7066 if 'stage +' in ln and 'seq=9' in ln]
        last = states[-1]
        listener = int(re.search(r'listener=([0-9A-F]{8})', last).group(1), 16)
        gmode = int(re.search(r'G\+18576=(\d+)', last).group(1))
        check(listener == 0xA4B79820 and gmode == 4 and any('sequence 9 -> 75' in ln for ln in lines7066[lines7066.index(last):]), 'run 7066 fixture: the last sequence-9 state is not the expected one')
        # the leave guard on that state (P+8 is not in the log: modelled as 1 = a root that pre-dates the stage, the crash class; R/Q words as in the 7072-like stage)
        crash_state = leave_line(g_mode=gmode, p=listener, p_owns=1)
        ok_state = leave_line(g_mode=gmode, p=listener, p_owns=0)
        got = run_exe(exe, [crash_state, ok_state])
        check(got[0].startswith('NOT ') and 'pre-dates' in got[0] and got[1] == 'OK', 'leave guard on the run 7066 words: P+8 = 1 must be refused with the crash-class reason, P+8 = 0 accepted: %s' % got)
        twin_exe = build_logic(twins['the P+8 guard dropped'], tmp, 'lane72_p8')
        check(run_exe(twin_exe, [crash_state])[0] == 'OK', 'planted twin: with the P+8 guard dropped the crash-class state must pass (so the real guard is what refuses it)')
        # sequence 75 with the listener registered = the 7066 crash precondition; a clean orderly end is not
        for what, text in (('the real header', header), ('listener ignored', twins['the listener ignored at the menu'])):
            e = exe if what == 'the real header' else build_logic(text, tmp, 'lane72_back')
            g = run_exe(e, ['back %d 0 0 0' % listener, 'back 0 0 0 0'])
            if what == 'the real header':
                check(g[0].startswith('LEFT') and g[1] == 'CLEAN', 'run 7066: sequence 75 with listener A4B79820 must be flagged, an all-zero end must be clean: %s' % g)
            else:
                check(g[0] == 'CLEAN', 'planted twin (listener ignored) must miss the run 7066 precondition')

    # ---- 8b/11. run 012 and run 7072 ----
    def opens(lines):
        out = []
        for ln in lines:
            m = re.search(r"NtCreateFile 'D:\\PAC\\([A-Z]+)\\([A-Z0-9_]+)\.PAC' open .* frame=(\d+)", ln)
            if m:
                out.append((m.group(1), m.group(2), int(m.group(3))))
        return out

    ed = open(os.path.join(fixtures_dir, 'editor_run012_bg_lines.txt'), encoding='utf-8').read().split('\n')
    o = opens(ed)
    bgs = [(n, f) for d, n, f in o if d == 'BG']
    inlua = [f for d, n, f in o if n == 'INGAMELUA']
    # the rule (design Q1: reload when SL+36 != G+64): every arena change in the editor's run reloaded, with no INGAMELUA reopened in between (inside sequence 9)
    reloads = [bgs[i][0] for i in range(1, len(bgs)) if bgs[i][0] != bgs[i - 1][0]]
    check([n for n, _ in bgs] == ['BG76', 'BG01', 'BG78'] and len(reloads) == 2 and len(inlua) == 1 and inlua[0] < bgs[0][1] < bgs[1][1] < bgs[2][1],
          'run 012: the editor loads BG76, BG01, BG78 after one INGAMELUA open (an arena change reloads inside the stage): %s %s' % (bgs, inlua))
    mine = open(os.path.join(fixtures_dir, 'stage_run7072_arena_lines.txt'), encoding='utf-8').read().split('\n')
    mine = [ln for ln in mine if ln.strip()]
    pushes = [(int(re.search(r'pushed scene (\d+)', ln).group(1)), int(re.search(r'\(arena (\d+),', ln).group(1)), ln) for ln in mine if 'play: pushed scene ' in ln]
    disp = [ln for ln in mine if 'play: dispatched scene ' in ln]
    bg7072 = [(n, f) for d, n, f in opens(mine) if d == 'BG']
    first_push = next(i for i, ln in enumerate(mine) if 'play: pushed scene ' in ln)
    bg_idx = next(i for i, ln in enumerate(mine) if "\\PAC\\BG\\BG76.PAC" in ln)
    check([p[:2] for p in pushes] == [(4001, 76), (5000, 76)] and len(disp) == 2 and all('arena=76' in d for d in disp) and bg7072 and [n for n, _ in bg7072] == ['BG76'] and bg_idx < first_push,
          'run 7072: both pushes asked for arena 76 = the arena BG76 the stage had loaded (one BG open, before the first push, none after a dispatch): %s %s' % (pushes[:2], bg7072))
    check(all('vtable 00000000' in d for d in disp) and any('cmd vtable=8200A410' in ln for ln in mine), 'run 7072: the dispatched line read command vtable 0 while the state lines show 8200A410 (the log nit)')
    # the corrected line: no class on the dispatched line, a class line when the vtable is non-zero (stage_log reads it)
    twin = mine + ['[2026-11-12 11:09:30.210] [info] [core] [t1] [wwe2k14][scene][director] play: command class of scene 5000: vtable 8200A410 (story class: holds at its end until the next push), frame=30200']
    res = stage_log.analyse(twin)
    check([d['id'] for d in res['dispatched']] == [4001, 5000] and res['dispatched'][1]['story'] and not res['dispatched'][0]['story'], 'stage_log must attach the later `play: command class` line to its dispatch: %s' % res['dispatched'])

    # ---- the confirmed leave sequence (run 7073, Check E3) ----
    l7073 = [ln for ln in open(os.path.join(fixtures_dir, 'leave_run7073_lines.txt'), encoding='utf-8').read().splitlines() if ln.strip()]
    got = leave_sequence_problems(l7073)
    check(not got, 'run 7073 leave sequence fixture: %s' % got)
    bad_l = [ln.replace('P+4=3', 'P+4=2', 1) if i == 6 else ln for i, ln in enumerate(l7073)]
    bad_l = [ln.replace('L=00000000', 'L=A4BF8F30') for ln in l7073]
    check(bool(leave_sequence_problems(bad_l)), 'planted fault (L never reaches 0) must break the leave sequence check')
    check(bool(leave_sequence_problems([ln for ln in l7073 if 'M+20=3' not in ln])), 'planted fault (M+20 3 missing) must break the leave sequence check')

    # ---- 10. text pins ----
    src, args_h = rd('src', 'scene_director.h'), rd('src', 'scene_director_args.h')
    got = static_problems(src, args_h, ss_mod)
    check(not got, 'lane 72 text pins on src/scene_director.h: %s' % got[:4])
    stw = planted_twins_static(src)
    for what, text in stw.items():
        if text == src:
            check(False, 'planted text twin "%s" did not change the file (the test lost its anchor)' % what)
            continue
        check(bool(static_problems(text, args_h, ss_mod)), 'planted lane 72 text fault (%s) was not caught' % what)

    # ---- 13. the parse rule knows leave ----
    cases = [('leave', ('LEAVE',)), ('  leave ', ('LEAVE',)), ('leave now', ('ERR',)), ('leave 4001', ('ERR',)), ('leave arena=4', ('ERR',)), ('Leave', ('ERR',)), ('play 4001 arena=78 324', ('PLAY', 4001, 78, 1, 324, 0))]
    with tempfile.TemporaryDirectory() as tmp:
        exe = scene_selftest_module.build_args_test(args_h, tmp, 'lane72_real')
        bad = scene_selftest_module.check_parse(exe, cases)
        check(not bad, 'the parse rule and `leave`: %s' % bad[:3])
        tw = scene_selftest_module.check_parse(scene_selftest_module.build_args_test(args_h.replace('if (w.size() != 1) return "error: leave: takes no arguments";', ''), tmp, 'lane72_twin'), cases)
        check(bool(tw), 'planted parse fault (leave takes arguments) was not caught')

    # ---- 12. listing pins ----
    main = resolve.main_checkout()
    listings = {fn: listing(main, fn) for fn in sorted({p[0] for p in PINS})}
    pp = pin_problems(listings, PINS)
    check(not pp, 'listing pins: %s' % pp[:3])
    for i in (0, 6, 10, 14, 20):
        fn, addr, text = PINS[i]
        wrong = list(PINS)
        wrong[i] = (fn, addr, text + '_planted')
        check(bool(pin_problems(listings, wrong)), 'planted listing pin %d was not caught' % i)
    print('8-13 (lane 72). arena: ArenaLoadable (clang++) answers 0-300 like the design and like the player\'s %s PAC\\BG files (%d ids), the python mirror and the plan carry arena=N, 76 says "the editor\'s empty stage"; '
          'leave: %d refused states + the run 7072 words OK, the run 7066-class state (P+8 = 1) refused and the dropped-guard twin passes it; run 7066 sequence 75 with the listener registered is flagged; '
          'runs 012 / 7072 re-found (an arena change reloads inside sequence 9, the same arena does not); text pins on the director with %d planted text faults, %d planted logic faults, %d listing pins' % (
              'read' if files else 'MISSING', len(files or []), len(LEAVE_BAD), len(stw), len(twins), len(PINS)))
