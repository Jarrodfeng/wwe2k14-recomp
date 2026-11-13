"""S101 lane 38 (the stage push: play any scene from the main menu): the offline acceptance test (gate lane scene_stage). No game run.

    py -I tools/goto/stage_selftest.py

1. Addresses: scene:ID[:CHAR[/ATTIRE],...] round-trips; malformed ones are refused with a reason; scene:ID plans stage -> ready -> play -> dispatched
   and every id in the bank table plans (an id no pac holds is an ERROR, never a plan).
2. The game's own parse rule (src/scene_director_args.h, COMPILED with clang++ via tools/goto/scene_args_test.cpp) answers the exact strings the executor
   builds (resolve.scene_arg stage / play) and typos; planted rule faults must be caught.
3. The shipped stage logic (src/scene_stage_logic.h, COMPILED with clang++ via tools/goto/scene_stage_test.cpp, the header src/scene_director.h includes):
   the ready test refuses every missing condition (play BEFORE READY is refused: R null, wrong class, a context queued, a push pending, ...); the 25,360-byte
   fake story's bytes are read back at the design's offsets, independent of the header's own constants; planted logic faults must be caught.
4. src/scene_director.h text pins (said as such; the file is compiled only by the game build, whose syntax-only compile is the builder's): FirePlay refuses a
   missing id and an unready stage BEFORE any write, never calls sub_82A30F08 with R == 0, writes in the handler's order, points M+40 at the story only for
   the call and puts it back (a missing restore is caught by planted faults), `!scene off` clears a pending flag it set.
5. A real past fact re-found from its log text: run 7058's stage reached WSD00 at frame 3385 and never logged `playback finished` (tools/goto/stage_log.py reads
   it, the log lines are kept in tools/goto/fixtures/); the proof-line format strings in scene_director.h are read by tools/goto/stage_log.py.
Exit 0 = all passed. Game files are read only through tools/goto/scene_ids.py; nothing from them is printed except counts.
"""
import os
import re
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import address  # noqa: E402
import resolve  # noqa: E402
import scene_selftest as ss  # noqa: E402
import stage_log  # noqa: E402

problems = []


def check(ok, what):
    if not ok:
        problems.append(what)
        print('FAIL: ' + what)
    return ok


def rd(*parts):
    return open(os.path.join(REPO, *parts), encoding='utf-8').read().replace('\r\n', '\n')


# ---------- 1. addresses and plans ----------
VALID = ['scene:4025', 'scene:4001:324,139', 'scene:4001:324/1,139/0', 'scene:5000:100', 'scene:6139:139/3,324', 'scene:4025:371/255',
         'scene:4025:324,139,100,101,102,103']
MALFORMED = ['scene:4025:', 'scene:4025:99', 'scene:4025:512', 'scene:4025:511', 'scene:4025:105', 'scene:4025:324/256', 'scene:4025:324/', 'scene:4025:/1', 'scene:4025:324,,139', 'scene:4025:a',
             'scene:4025:324,139,100,101,102,103,104', 'scene:4025:324:139', 'scene:4025:-1', 'scene:4025:324 139', 'scene:0:324', 'scene:65536:324',
             'scene:4025:324@bell', 'scene::324']


def addresses(ctx):
    for t in VALID:
        try:
            a = address.parse(t)
            b = address.parse(str(a))
        except address.AddressError as x:
            check(False, 'valid address refused: %s (%s)' % (t, x))
            continue
        check(a == b and str(a) == t, 'round trip changed %s: %s -> %s' % (t, a, b))
    a = address.parse('scene:4001:324/1,139')
    check(a.scene_ids == (4001,) and a.performers == ((324, 1), (139, -1)), 'scene fields: %s' % (a,))
    refused = 0
    for t in MALFORMED:
        try:
            address.parse(t)
            check(False, 'malformed address accepted: %r' % t)
        except address.AddressError as x:
            check(bool(str(x)), 'refusal without a reason: %r' % t)
            refused += 1
    ids = ctx.scene_ids['ids']
    want = ['nav', 'scene_stage', 'expect_scene_log', 'scene_play', 'expect_scene_log']
    bad = 0
    holding = 0
    for k, pac in ids.items():
        p = resolve.resolve('scene:%s:324/1,139' % k, ctx)
        if not (1 <= int(k) <= 699 or 4000 <= int(k) <= 5999):  # S101 lane 59: a bank that does not hold in the stage is not reachable (STAGE_CRASH.md)
            check(p.status == 'not_reachable' and not p.steps, 'scene:%s must be not_reachable, got %s' % (k, p.status))
            continue
        holding += 1
        ok = (p.status == 'plan' and [s['op'] for s in p.steps] == want and p.steps[1] == {'op': 'scene_stage'} and p.steps[2]['text'] == 'stage: ready' and
              p.steps[3]['id'] == int(k) and p.steps[3]['pac'] == pac and p.steps[3]['performers'] == [[324, 1], [139, -1]] and
              p.steps[4]['text'] == 'play: dispatched scene %s ' % k)
        bad += not ok
    check(bad == 0, '%d bank ids did not plan stage -> ready -> play -> dispatched' % bad)
    for g, want_status in ((6139, 'not_reachable'), (4001, 'plan'), (5000, 'plan'), (1010, 'not_reachable')):
        if str(g) in ids:
            check(resolve.resolve('scene:%d:139' % g, ctx).status == want_status, 'scene:%d:139 must be %s' % (g, want_status))
    check(resolve.resolve('scene:4001:324,139', ctx).status == 'plan', 'scene:4001:324,139 must still plan')
    missing = [g for g in (6385, 3999, 6324) if str(g) not in ids]
    for g in missing:
        p = resolve.resolve('scene:%d:324' % g, ctx)
        check(p.status == 'error' and not p.steps, 'scene:%d (in no pac) must be an error, got %s' % (g, p.status))
    # the executor's request text for each step is the string the game parses
    p = resolve.resolve('scene:4001:324/1,139', ctx)
    check(resolve.describe_step(p.steps[3]).endswith('!scene play 4001 324/1 139 arena=1'), 'describe_step(scene_play): %s' % resolve.describe_step(p.steps[3]))
    print('1. addresses: %d valid scene:ID[:CHARS] forms round-trip, %d malformed refused; %d of %d bank ids (1-699, 4000-5999) plan stage -> ready -> play -> dispatched, the rest are not_reachable (6139 included); %d ids in no pac are errors' % (
        len(VALID), refused, holding, len(ids), len(missing)))


# ---------- 2. the parse rule ----------
def parse_cases():
    good = [(resolve.scene_arg('stage', []), ('STAGE',)),
            (resolve.scene_arg('play', [4025]), ('PLAY', 4025, -1, 0)),
            (resolve.scene_arg('play', [4001], performers=[(324, 1), (139, -1)]), ('PLAY', 4001, -1, 2, 324, 1, 139, 0)),
            (resolve.scene_arg('play', [5000], performers=[(324, 0)], arena=76), ('PLAY', 5000, 76, 1, 324, 0)),
            ('play 65535 511/255', ('PLAY', 65535, -1, 1, 511, 255)), ('play 1 100 101 102 103 104 105', ('PLAY', 1, -1, 6, 100, 0, 101, 0, 102, 0, 103, 0, 104, 0, 105, 0)),
            ('play 4025 arena=0 324', ('PLAY', 4025, 0, 1, 324, 0)), ('play 4025 324 arena=199', ('PLAY', 4025, 199, 1, 324, 0)),
            ('  stage  ', ('STAGE',)), ('stage', ('STAGE',))]
    bad = ['stage 1', 'stage force', 'play', 'play 0', 'play 65536', 'play 12a', 'play -5', 'play 4025 99', 'play 4025 512', 'play 4025 324/256', 'play 4025 324/',
           'play 4025 /1', 'play 4025 324/-1', 'play 4025 1000', 'play 4025 arena=200', 'play 4025 arena=', 'play 4025 arena=x', 'play 4025 arena=1 arena=2',
           'play 4025 100 101 102 103 104 105 106', 'play 4025 force', 'play 4025,4026', 'play 4025 324,139', 'play 4025 mode=2', 'stage 4025', 'STAGE', 'Play 4025']
    return good + [(b, ('ERR',)) for b in bad]


def parse_rule():
    header = rd('src', 'scene_director_args.h')
    cases = parse_cases()
    with tempfile.TemporaryDirectory() as tmp:
        exe = ss.build_args_test(header, tmp, 'real')
        bad = ss.check_parse(exe, cases)
        check(not bad, 'the game parse rule disagrees with what the executor builds / the typo list: %s' % bad[:4])
        twins = {
            'character floor lowered': header.replace('constexpr int kMinChar = 100;', 'constexpr int kMinChar = 0;'),
            'performer cap raised': header.replace('constexpr int kMaxPerformers = 6;', 'constexpr int kMaxPerformers = 7;'),
            'arena cap raised': header.replace('constexpr int kMaxArena = 199;', 'constexpr int kMaxArena = 255;'),
            'attire cap raised': header.replace('constexpr int kMaxAttire = 255;', 'constexpr int kMaxAttire = 999;'),
            'stage takes arguments': header.replace('if (w.size() != 1) return "error: stage: takes no arguments";', ''),
            'arena twice allowed': header.replace('if (out.arena >= 0) return "error: play: arena given twice";', ''),
        }
        for what, text in twins.items():
            if text == header:
                check(False, 'planted twin "%s" did not change the header' % what)
                continue
            tb = ss.check_parse(ss.build_args_test(text, tmp, 'twin_' + re.sub(r'\W', '', what)), cases)
            check(bool(tb), 'planted parse fault (%s) was not caught' % what)
    print('2. parse rule: the real src/scene_director_args.h (clang++) answered %d strings right (%d the executor builds / valid forms, %d typos refused); %d planted rule faults caught' % (
        len(cases), sum(1 for c in cases if c[1][0] != 'ERR'), sum(1 for c in cases if c[1][0] == 'ERR'), len(twins)))


# ---------- 3. the shipped logic ----------
STORY = 25360
DESIGN = {  # the design's numbers (GOANY_scene_from_menu.md), written here independently of the header
    'kFakeStorySize': 21248 + 4112, 'kBodyOffset': 21248 + 8, 'kRingRead': 20, 'kRingWrite': 24, 'kCtxBase': 28, 'kCtxSize': 332, 'kCtxSlot0': 140, 'kCtxType': 8,
    'kCtxArena': 16, 'kPending': 5464, 'kBodyValid': 5456 + 4116, 'kStoryListener': 5452, 'kStoryClassObj': 9632, 'kStoryClassVtable': 0x8200A410, 'kGameMode': 18576,
}
# a ready stage: held 0, seq 9, R, G+18576 = 4, cmd obj, story vtable, read 3, write 4, slot0 4025, listener, pending 0, M, participants
READY = [0, 9, 0x40001000, 4, 0x40002000, 0x8200A410, 3, 4, 4025, 0x40003000, 0, 0x40004000, 0x40005000, 0, 0]


def ready_line(**kw):
    names = ['held', 'seq', 'r', 'g_mode', 'cmd_obj', 'cmd_vtable', 'read', 'write', 'slot0', 'listener', 'pending', 'm', 'part', 'ui', 'gate']
    v = dict(zip(names, READY))
    v.update(kw)
    return 'ready ' + ' '.join(str(v[n]) for n in names)


def story_line(bank, arena, perf, verb='story'):
    return '%s %d %d %d' % (verb, bank, arena, len(perf)) + ''.join(' %d %d' % p for p in perf)


def build_logic(header, tmp, tag):
    inc = os.path.join(tmp, tag)
    os.makedirs(inc, exist_ok=True)
    with open(os.path.join(inc, 'scene_stage_logic.h'), 'w', encoding='utf-8') as f:
        f.write(header)
    exe = os.path.join(inc, 'scene_stage_test.exe')
    r = subprocess.run([ss.find_clang(), '-std=c++17', '-I', inc, os.path.join(HERE, 'scene_stage_test.cpp'), '-o', exe], capture_output=True, text=True, timeout=180)
    if r.returncode != 0 or not os.path.isfile(exe):
        raise RuntimeError('scene_stage_test did not compile: %s' % (r.stdout + r.stderr)[-600:])
    return exe


HOLD = (1, 324, 699, 4000, 4001, 4025, 5000, 5999)
ENDS = (0, 700, 999, 1000, 3999, 6000, 6139, 6343, 9500, 10000, 65535)


def holds_problems(exe):
    """S101 lane 59: HoldsInStage answers (the game's sub_8231C538 rule for type-4 contexts)."""
    banks = list(HOLD) + list(ENDS)
    out = subprocess.run([exe], input=''.join('holds %d\n' % b for b in banks), capture_output=True, text=True, timeout=60).stdout.split()
    p = []
    if len(out) != len(banks):
        return ['holds: %d answers for %d banks' % (len(out), len(banks))]
    for b, g in zip(banks, out):
        want = 'HOLD' if b in HOLD else 'ENDS'
        if g != want:
            p.append('holds %d: want %s, got %s' % (b, want, g))
    return p


def logic_problems(exe):
    """Problems in the logic the exe was built from (the real header must have none; every planted twin at least one)."""
    p = []
    lines = [ready_line(),                                  # 0 READY
             ready_line(held=0, slot0=4001),                # 1 the held bank is not accepted without held_bank
             ready_line(held=4001, slot0=4001),             # 2 accepted with it
             ready_line(held=4001, slot0=4002),             # 3 another bank is not
             ready_line(seq=75), ready_line(r=0), ready_line(g_mode=3), ready_line(cmd_obj=0), ready_line(cmd_vtable=0x8200A3C0),
             ready_line(read=10), ready_line(write=10), ready_line(write=5), ready_line(write=0), ready_line(slot0=0),
             ready_line(listener=0), ready_line(pending=1), ready_line(m=0), ready_line(part=0),
             ready_line(read=9, write=0),                   # wraps: ready
             ready_line(ui=0x40006000, gate=0),             # the UI manager exists and its +156 is 0: the stage would not hold
             ready_line(ui=0x40006000, gate=1),             # ... and with +156 set: ready
             'consts']
    out = subprocess.run([exe], input='\n'.join(lines) + '\n', capture_output=True, text=True, timeout=60).stdout.split('\n')
    got = out[:len(lines) - 1]
    want = ['READY', 'NOT', 'READY', 'NOT'] + ['NOT'] * 14 + ['READY', 'NOT', 'READY']
    for i, (g, w) in enumerate(zip(got, want)):
        if g.split(' ')[0] != w:
            p.append('ready case %d (%s): want %s, got %s' % (i, lines[i], w, g[:60]))
    if len(got) != len(want):
        p.append('ready: %d answers for %d cases' % (len(got), len(want)))
    consts = {}
    for ln in out[len(want):]:
        if '=' in ln:
            k, v = ln.split('=', 1)
            consts[k] = int(v)
    for k, v in DESIGN.items():
        if consts.get(k) != v:
            p.append('constant %s is %s, the design says %s' % (k, consts.get(k), v))
    return p


def story_bytes(exe, bank, arena, perf, verb='story'):
    r = subprocess.run([exe], input=story_line(bank, arena, perf, verb) + '\n', capture_output=True, text=True, timeout=60).stdout.strip()
    if not r.startswith('OK '):
        return None
    return bytes.fromhex(r[3:])


def u16(b, o):
    return b[o] * 256 + b[o + 1]


def story_problems(exe):
    p = []
    perf = [(324, 1), (139, 0), (343, 255)]
    f = story_bytes(exe, 4001, 76, perf, 'storyd')  # the game's performer defaults (255 / 255 at +4 / +5) are in the buffer before we fill it
    if f is None or len(f) != STORY:
        return ['the fake story is not %d bytes (%s)' % (STORY, None if f is None else len(f))]
    body = 21248 + 8  # record = S+21248+4112k, body = record+8 (design Q1 table)
    if u16(f, 88) != 0:
        p.append('S+88 (selection) is not 0')
    if u16(f, 1248) != 0:
        p.append('S+1248 (scene index table) is not 0')
    if any(f[1100:1104]):
        p.append('S+1100 (variant) is not 0')
    if f[21248] != 0:
        p.append('record+0 is not 0')
    if u16(f, body) != 4001:
        p.append('body+0 (bank) is %d, want 4001' % u16(f, body))
    if u16(f, body + 2) != 76:
        p.append('body+2 (arena) is %d, want 76' % u16(f, body + 2))
    if f[body + 5] != 3:
        p.append('body+5 (performer count) is %d, want 3' % f[body + 5])
    for j, (c, a) in enumerate(perf):
        e = body + 16 + 12 * j
        want = [c >> 8, c & 255, a, 0xFF, 0xFF, 0xFF, 2, 0]  # +4 / +5 keep the defaults (not overwritten with 0)
        if list(f[e:e + 8]) != want:
            p.append('performer %d at body+%d is %s, want %s' % (j, 16 + 12 * j, list(f[e:e + 8]), want))
        if any(f[e + 8:e + 12]):
            p.append('performer %d trailing bytes are not 0' % j)
    # nothing outside the fields we mean to write
    mask = {body + 16 + 12 * j + k for j in range(6) for k in (4, 5)} | set(range(88, 90)) | set(range(1248, 1250)) | set(range(body, body + 2)) | set(range(body + 2, body + 4)) | {body + 5} | set(range(body + 16, body + 16 + 36))
    stray = [i for i, x in enumerate(f) if x and i not in mask]
    if stray:
        p.append('the fake story has stray non-zero bytes outside its fields at %s' % stray[:6])
    # a larger arena / zero performers
    g = story_bytes(exe, 65535, 199, [])
    if g is None or u16(g, body) != 65535 or g[body + 5] != 0 or u16(g, body + 2) != 199:
        p.append('an empty-performer story is wrong')
    for refused in ((0, 76, []), (4001, 76, [(324, 1)] * 7), (70000, 76, [])):
        if story_bytes(exe, *refused) is not None:
            p.append('FillStory accepted %s' % (refused,))
    return p


def logic():
    header = rd('src', 'scene_stage_logic.h')
    with tempfile.TemporaryDirectory() as tmp:
        exe = build_logic(header, tmp, 'real')
        bad = logic_problems(exe) + story_problems(exe) + holds_problems(exe)
        check(not bad, 'the shipped stage logic: %s' % bad[:4])
        twins = {
            'play with R null is ready': header.replace('if (w.r == 0) return', 'if (false) return'),
            'any command class is the story class': header.replace('if (w.cmd_vtable != kStoryClassVtable) return', 'if (false) return'),
            'a push already pending is ready': header.replace('if (w.pending != 0) return', 'if (false) return'),
            'a queued context is ready': header.replace('if (w.ring_write != (w.ring_read + 1) % kRingSlots) return', 'if (false) return'),
            'the held bank is always accepted': header.replace('!(held_bank != 0 && w.slot0 == held_bank)', 'false'),
            'stage bank 4025 not required': header.replace('if (w.slot0 != kStageBank &&', 'if (false &&'),
            'non-story mode is ready': header.replace('if (w.g_mode != kGameModeStory) return', 'if (false) return'),
            'a UI gate of 0 is ready': header.replace('if (w.ui_manager != 0 && w.ui_gate == 0) {', 'if (false) {'),
            'story root may be null': header.replace('if (w.story_root == 0) return', 'if (false) return'),
            'body offset moved': header.replace('kBodyOffset = kRecordOffset + 8;', 'kBodyOffset = kRecordOffset + 4;'),
            'pending offset wrong': header.replace('kPending = 5456 + 8;', 'kPending = 5456 + 4;'),
            'arena not written': header.replace('PutU16(body + kBodyArena, arena);', ''),
            'defaults overwritten with 0': header.replace('    e[3] = 0xFF;', '    e[3] = 0xFF;\n    e[4] = 0;\n    e[5] = 0;'),
            'override byte not -1': header.replace('e[3] = 0xFF;', 'e[3] = 0;'),
            'linked character not none': header.replace('PutU16(e + 6, kPerformerNone);', 'PutU16(e + 6, 0);'),
            'story size changed': header.replace('kFakeStorySize = 25360;', 'kFakeStorySize = 25344;'),
            'too many performers accepted': header.replace('n > int(kMaxPerformers)', 'n > 40'),
            'holds: upper bound 6999': header.replace('bank <= 5999', 'bank <= 6999'),
            'holds: 1-699 branch dropped': header.replace('(bank >= 1 && bank <= 699) || ', ''),
            'holds: < 5999': header.replace('bank <= 5999', 'bank < 5999'),
        }
        for what, text in twins.items():
            if text == header:
                check(False, 'planted twin "%s" did not change the header' % what)
                continue
            try:
                texe = build_logic(text, tmp, 'twin_' + re.sub(r'\W', '', what))
                got = logic_problems(texe) + story_problems(texe) + holds_problems(texe)
            except RuntimeError:
                got = ['does not compile']
            check(bool(got), 'planted logic fault (%s) was not caught' % what)
    print('3. shipped logic: src/scene_stage_logic.h (clang++) refuses play before ready for each of 14 missing conditions, accepts the held bank and the wrapped ring, builds the %d-byte story '
          'with every field at the design offset and nothing else set; %d planted logic faults caught' % (STORY, len(twins)))


# ---------- 4. text pins on src/scene_director.h ----------
def static_problems(src, args_h):
    p = []

    def body(sig):
        return ss.body_of(src, sig)

    play, stage, tick = body('inline std::string FirePlay('), body('inline std::string FireStage('), body('inline void StageTick(')
    cancel, main_tick = body('inline void StageCancel('), body('inline void Tick(')
    live_cmd, menu = body('inline std::string LiveCommand('), body('inline std::string FireMenu(')
    reads = body('inline scene_stage::StageWords ReadStageWords(')
    for name, b in (('FirePlay', play), ('FireStage', stage), ('StageTick', tick), ('ReadStageWords', reads)):
        if not b:
            p.append('%s not found' % name)
    if not play:
        return p
    calls = re.findall(r'call\((sub_[0-9A-F]{8})', play)
    if calls != ['sub_82A22580', 'sub_8231A5D0', 'sub_82A30F08']:
        p.append('FirePlay calls %s, want the handler recipe [defaults, body-valid word, sub_82A30F08]' % calls)
    if set(ss.FUNC_RE.findall(play)) - {'sub_82A22580', 'sub_8231A5D0', 'sub_82A30F08'}:
        p.append('FirePlay names functions outside its recipe')
    i = {k: play.find(k) for k in ('BankExists(call', 'HoldsInStage(uint32_t(id))', 'ReadStageWords()', 'NotReady(w, g_held_bank)', 'if (r == 0 || m == 0)', 'SystemHeapAlloc', 'std::memset(Guest(g_fake_story)',
                                   'call(sub_82A22580', 'if (arena > 65535) return "refused: the stage arena is unset', 'FillStory(', 'call(sub_8231A5D0', 'StagePut32(cam + ss::kCamFlag, 1)', 'const uint32_t old_story = U32(m + ss::kStoryOffsetInRoot);',
                                   'StagePut32(m + ss::kStoryOffsetInRoot, g_fake_story)', 'call(sub_82A30F08', 'StagePut32(m + ss::kStoryOffsetInRoot, old_story)')}
    for k, v in i.items():
        if v < 0:
            p.append('FirePlay lost %s' % k)
    order = list(i)
    if all(v >= 0 for v in i.values()) and [i[k] for k in order] != sorted(i[k] for k in order):
        p.append('FirePlay steps are out of order (id test, ready test, R/M null test, allocation, zero, defaults, fill, body-valid, camera flag, save M+40, point M+40, '
                 'sub_82A30F08, restore M+40)')
    if 'return "refused: /EVP/"' not in play or 'return "refused: the stage is not ready: "' not in play or 'return "refused: no stage requested' not in play:
        p.append('FirePlay must refuse a missing id, a stage not requested and a stage not ready')
    if play.count('call(sub_82A30F08') != 1:
        p.append('FirePlay must call sub_82A30F08 exactly once')
    if 'U32(m + ss::kStoryOffsetInRoot) != g_fake_story' not in play or 'the call changed M+40' not in play or \
            play.find('the call changed M+40') > play.find('StagePut32(m + ss::kStoryOffsetInRoot, old_story)'):
        p.append('FirePlay must check, before restoring, that the call left M+40 at our story (WARN "the call changed M+40")')
    if 'g_menu_seen' not in play or 'g_stage_on' not in play:
        p.append('FirePlay must wait for the main menu and a requested stage')
    if 'FireMenu(ctx, base, m)' not in stage or 'call(' in stage or 'kStageBank' not in stage or 'm.mode = 2' not in stage:
        p.append('FireStage must be the menu verb with bank 4025 and mode 2 and nothing else')
    if 'g_stage_on = true;' not in stage or stage.find('FireMenu(') > stage.find('g_stage_on = true;'):
        p.append('FireStage may mark the stage only after the menu request was accepted')
    # `!scene off`
    if 'g_cancel_stage.store(true)' not in live_cmd or 'g_cancel_stage.exchange(false)' not in cancel:
        p.append('`!scene off` must reach StageCancel (g_cancel_stage)')
    if 'g_play_pending_ours' not in cancel or 'StagePut32(r + ss::kPending, 0)' not in cancel:
        p.append('`!scene off` must clear the pending flag the push set')
    if 'StageCancel();' not in main_tick or main_tick.find('StageCancel();') > main_tick.find('g_pending.exchange'):
        p.append('Tick must serve `!scene off` (StageCancel) before it serves a request of the same frame')
    if 'StageCancel();' in tick:
        p.append('StageTick must not serve the cancel (it runs after the requests)')
    if 'ui_gate' not in reads or 'kUiGate' not in reads:
        p.append('ReadStageWords must read the UI manager gate')
    if not re.search(r'case scene::Verb::kStage:[^{]*case scene::Verb::kPlay:(\s*case scene::Verb::kLeave:)? \{', live_cmd) or 'kStage ? 5 : a.verb == scene::Verb::kPlay ? 6' not in live_cmd:
        p.append('LiveCommand must queue stage (5) and play (6) like the other verbs')
    if re.search(r'U32\(|Guest\(|TranslateVirtual|SequenceId\(|ReadStageWords', live_cmd):
        p.append('LiveCommand reads guest memory on the pipe thread')
    if 'kind == 5 ? FireStage(ctx, base, a) : kind == 6 ? FirePlay(ctx, base, a)' not in src or 'StageTick();' not in body('inline void Tick('):
        p.append('Tick must serve kinds 5 / 6 and call StageTick')
    # the ready test reads exactly the words the design names
    for need in ('kStoryClassObj', 'kRingRead', 'kRingWrite', 'kCtxSlot0', 'kStoryListener', 'kPending', 'kStoryRootPtr', 'kParticipantsPtr', 'kGameMode'):
        if need not in reads:
            p.append('ReadStageWords does not read %s' % need)
    # proof lines, as the main session greps them
    for text in ('stage: ready R=', 'play: pushed scene ', 'play: committed', 'play: dispatched scene ', 'play: scene list done after scene ', 'stage: requested'):
        if text not in src:
            p.append('proof line missing: %s' % text)
    for need in ('#include "scene_stage_logic.h"', 'REX_EXTERN(sub_82A30F08);', 'REX_EXTERN(sub_82A22580);', 'REX_EXTERN(sub_8231A5D0);'):
        if need not in src:
            p.append('missing: %s' % need)
    if 'kStage, kPlay' not in args_h:
        p.append('the arg rule lacks stage / play')
    return p


def static_check():
    src, args_h = rd('src', 'scene_director.h'), rd('src', 'scene_director_args.h')
    got = static_problems(src, args_h)
    check(not got, 'static check of the stage push in src/scene_director.h: %s' % got[:4])
    twins = {
        'M+40 never restored': src.replace('  StagePut32(m + ss::kStoryOffsetInRoot, old_story);\n', '', 1),
        'M+40 restored before the call': src.replace('  call(sub_82A30F08, {g_caller_object});\n', '  StagePut32(m + ss::kStoryOffsetInRoot, old_story);\n  call(sub_82A30F08, {g_caller_object});\n', 1),
        'M+40 check reads back its own restore': src.replace('if (U32(m + ss::kStoryOffsetInRoot) != g_fake_story) REXLOG_WARN', 'if (U32(m + ss::kStoryOffsetInRoot) != old_story) REXLOG_WARN', 1),
        'cancel served after the requests': src.replace('  StageCancel();  // S101 lane 38: first, before any request of this frame is served\n', '', 1).replace('  StageTick();  // S101 lane 38\n', '  StageCancel();\n  StageTick();  // S101 lane 38\n', 1),
        'arena unset falls to the generic failure': src.replace('  if (arena > 65535) return "refused: the stage arena is unset', '  if (false) return "refused: the stage arena is unset', 1),
        'M+40 not verified': src.replace('if (U32(m + ss::kStoryOffsetInRoot) != g_fake_story) REXLOG_WARN', 'if (false) REXLOG_WARN', 1),
        'no missing-id refusal': src.replace('if (!BankExists(call, id)) {\n    return "refused: /EVP/" + std::to_string(id) + " does not resolve in the pac index (the game finds no scene bank',
                                             'if (false) {\n    return "refused: /EVP/" + std::to_string(id) + " does not resolve in the pac index (the game finds no scene bank', 1),
        'holds test removed': src.replace('  if (!ss::HoldsInStage(uint32_t(id))) {', '  if (false) {', 1),
        'holds test after the ready test': src.replace('  if (!ss::HoldsInStage(uint32_t(id))) {', '  if (false) {', 1).replace(
            '  const std::string why = ss::NotReady(w, g_held_bank);', '  const std::string why = ss::NotReady(w, g_held_bank);\n  if (!ss::HoldsInStage(uint32_t(id))) return "x";', 1),
        'no ready test': src.replace('const std::string why = ss::NotReady(w, g_held_bank);', 'const std::string why;', 1),
        'no R null guard': src.replace('if (r == 0 || m == 0) return "refused:', 'if (false) return "refused:', 1),
        'push before the ready test': src.replace('  const ss::StageWords w = ReadStageWords();\n  const std::string why = ss::NotReady(w, g_held_bank);',
                                                    '  call(sub_8231A5D0, {0, 1});\n  const ss::StageWords w = ReadStageWords();\n  const std::string why = ss::NotReady(w, g_held_bank);', 1),
        'body-valid word after the push': src.replace('  call(sub_8231A5D0, {r + ss::kBodyObj, 1});\n', '', 1).replace(
            '  call(sub_82A30F08, {g_caller_object});\n', '  call(sub_82A30F08, {g_caller_object});\n  call(sub_8231A5D0, {r + ss::kBodyObj, 1});\n', 1),
        'a second push call': src.replace('  call(sub_82A30F08, {g_caller_object});\n', '  call(sub_82A30F08, {g_caller_object});\n  call(sub_82A30F08, {0});\n', 1),
        'play without a requested stage': src.replace('if (!g_stage_on) return "refused: no stage requested', 'if (false) return "refused: no stage requested', 1),
        'stage marks itself before the menu request': src.replace('  const std::string answer = FireMenu(ctx, base, m);\n', '  g_stage_on = true;\n  const std::string answer = FireMenu(ctx, base, m);\n', 1),
        'stage uses mode 0': src.replace('  m.mode = 2;\n  const std::string answer = FireMenu', '  m.mode = 0;\n  const std::string answer = FireMenu', 1),
        'off does not clear the pending flag': src.replace('StagePut32(r + ss::kPending, 0);', '(void)r;', 1),
        'off never reaches the tick': src.replace('g_cancel_stage.store(true);', '', 1),
        'pipe thread reads guest memory for play': src.replace('      g_request = a;\n', '      g_request = a; (void)ReadStageWords();\n', 1),
        'tick does not serve play': src.replace('kind == 6 ? FirePlay(ctx, base, a)', 'kind == 6 ? std::string("armed")', 1),
        'stage tick never called': src.replace('  StageTick();  // S101 lane 38\n', '', 1),
        'ready test reads the wrong pending word': src.replace('w.pending = U32(w.r + kPending);', 'w.pending = U32(w.r + kBodyValid);', 1),
    }
    for what, text in twins.items():
        if text == src:
            check(False, 'planted twin "%s" did not change the file (the test lost its anchor)' % what)
            continue
        check(bool(static_problems(text, args_h)), 'planted fault (%s) was not caught' % what)
    print('4. text pins on src/scene_director.h: FirePlay = id test, ready test, R/M null test, then the handler\'s tail in its order with M+40 restored right after the call; '
          'FireStage = menu 4025 mode 2; `!scene off` clears a pending flag it set; %d planted text faults caught' % len(twins))


# ---------- 5. past fact + proof lines ----------
def past_fact():
    lines = open(os.path.join(HERE, 'fixtures', 'stage_run7058_lines.txt'), encoding='utf-8').read().split('\n')
    r = stage_log.analyse(lines)
    reached = [m for m in (re.search(r'"f": (\d+),.*WSD00', ln) for ln in lines) if m]
    check(r['requested'] == 3154, 'run 7058: the menu request was at frame 3154, the reader found %s' % r['requested'])
    check([int(m.group(1)) for m in reached] == [3184, 3385], 'run 7058: WSD00 loads at frames 3184 and 3385, found %s' % [m.group(1) for m in reached])
    check(not r['finished'], 'run 7058 never logged `menu: playback finished`, the reader says it did')
    check(r['ready'] is None and not r['dispatched'], 'run 7058 (the old verb) must show no stage / play lines')
    # the same reader sees a finish when there is one (a planted twin of the log)
    twin = lines + ['[2026-11-11 22:40:00.000] [info] [core] [t1] [wwe2k14][scene][director] menu: playback finished (R=40000000, R+9640=1); the story child menu now waits, seq=9, frame=4000']
    check(stage_log.analyse(twin)['finished'], 'planted twin: a `playback finished` line must be seen')
    # the original log, when it is still there (24 h): the whole file has no such line
    big = os.path.join(resolve.main_checkout(), 'out', 'build', 'win-amd64-release', 'logs', 'wwe2k14_7058.log')
    note = 'original log gone (24 h), fixture only'
    if os.path.isfile(big):
        text = open(big, encoding='utf-8', errors='replace').read()
        check('menu: playback finished' not in text and '"f": 3385' in text, 'the full 7058 log disagrees with the fixture')
        note = 'the full 3.5 MB log re-read: 0 `playback finished` lines'
    # the proof-line format strings in the director parse in stage_log (placeholders -> numbers)
    src = rd('src', 'scene_director.h')

    def render(prefix):
        m = re.search(r'REXLOG_INFO\("\[wwe2k14\]\[scene\]\[director\] (' + re.escape(prefix) + r'[^"]*)"', src)
        if not m:
            check(False, 'no REXLOG_INFO with the proof line "%s" in scene_director.h' % prefix)
            return ''
        t = re.sub(r'\{:?[0-9A-Za-z]*\}', '7', m.group(1))
        return '[2026-11-11 23:00:00.000] [info] [core] [t1] [wwe2k14][scene][director] ' + t

    fake = [render('stage: requested'), render('stage: ready'), render('play: pushed scene '), render('play: committed'), render('play: dispatched scene '),
            render('play: scene list done after scene ')]
    a = stage_log.analyse(fake)
    check(a['requested'] == 7 and a['ready'] == 7 and [x['id'] for x in a['pushes']] == [7] and a['committed'] == [7] and [x['id'] for x in a['dispatched']] == [7] and
          [x['id'] for x in a['done']] == [7], 'stage_log.py does not read the director\'s own proof-line format strings: %s' % a)
    print('5. past fact: run 7058 (the old menu 4025 mode 2): WSD00 loaded at frames 3184 and 3385, request at 3154, no `playback finished` (%s; a planted finish line is seen); '
          'stage_log.py reads the director\'s own proof-line formats' % note)


# ---------- 6. S101 lane 59: stage.py wait finds a line logged BEFORE the wait process started (Check E run 7066: 4 of 4 waits timed out) ----------
def wait_check():
    import stage
    lines = [ln for ln in open(os.path.join(HERE, 'fixtures', 'stage_run7066_play_lines.txt'), encoding='utf-8').read().split('\n') if ln]
    pushed = next((i for i, ln in enumerate(lines) if 'play: pushed scene 4001' in ln), -1)
    check(pushed >= 0 and any('play: dispatched scene 4001 ' in ln for ln in lines[pushed:]), 'the run 7066 fixture lacks pushed / dispatched lines')
    with tempfile.TemporaryDirectory() as d:
        log = os.path.join(d, 'run.log')
        before = ''.join(ln + '\n' for ln in lines[:pushed])
        after = ''.join(ln + '\n' for ln in lines[pushed:])
        with open(log, 'w', encoding='utf-8', newline='') as f:
            f.write('[2026-11-12 10:00:00.000] [info] [core] [t1] noise line\n' + before)
        stage.live.OUT = d  # the mark file goes in the temp folder
        mark_at = os.path.getsize(log)
        with open(stage._mark_file('t'), 'w', encoding='utf-8') as f:
            f.write('{"run": 7066, "offset": %d, "verb": "!scene play 4001", "time": %r}' % (mark_at, time.time()))  # what mark() stores before the verb is sent
        with open(log, 'a', encoding='utf-8', newline='') as f:
            f.write(after)  # the verb ran: pushed / committed / dispatched are all in the log before the wait process starts
        size_now = os.path.getsize(log)
        # the old behaviour (start = the size when the wait began) cannot see the line; the mark can
        check(stage.scan(log, size_now, 'play: dispatched scene 4001 ') is None, 'planted old behaviour (start at the size now) must miss the line')
        off = stage.start_offset('t', 7066, size_now)
        check(off == mark_at, 'the wait does not start at the verb\'s mark: %d vs %d' % (off, mark_at))
        hit = stage.scan(log, off, 'play: dispatched scene 4001 ')
        check(bool(hit) and 'dispatched scene 4001' in hit, 'wait from the mark did not find `play: dispatched scene 4001`')
        check(stage.scan(log, off, 'play: dispatched scene 9999 ') is None, 'a scene id that was not pushed must not match')
        # a mark of another run, or one past the end of the log, is ignored (the wait then starts at the log size now)
        check(stage.start_offset('t', 7067, size_now) == size_now, 'a mark of another run must be ignored')
        with open(stage._mark_file('t'), 'w', encoding='utf-8') as f:
            f.write('{"run": 7066, "offset": %d, "verb": "x", "time": %r}' % (size_now + 10, time.time()))
        check(stage.start_offset('t', 7066, size_now) == size_now, 'a mark past the end of the log must be ignored')
        check(stage.start_offset('none', 7066, size_now) == size_now, 'no mark file: start at the size now')
        with open(stage._mark_file('t'), 'w', encoding='utf-8') as f:  # a stale mark (older than the limit) is ignored
            f.write('{"run": 7066, "offset": %d, "verb": "!scene play 4001", "time": %r}' % (mark_at, time.time() - stage.MARK_MAX_AGE_S - 5))
        check(stage.start_offset('t', 7066, size_now) == size_now, 'a mark older than %d s must be ignored' % stage.MARK_MAX_AGE_S)
    print('6. stage.py wait (Check E run 7066): from the log size before the verb the `play: dispatched scene 4001` line of the run 7066 fixture is found even when written before '
          'the wait started; the planted old start (size at wait start) misses it; a mark of another run / past the end is ignored')


# ---------- 7. S101 lane 59: the run 7066 crash re-found from its log lines (docs/research/s101/STAGE_CRASH.md) ----------
def crash_check():
    lines = open(os.path.join(HERE, 'fixtures', 'stage_run7066_lines.txt'), encoding='utf-8').read().split('\n')
    dispatched = [int(m.group(1)) for m in (re.search(r'play: dispatched scene (\d+) ', ln) for ln in lines) if m]
    vtables = [m.group(1) for m in (re.search(r'cmd vtable=([0-9A-F]{8})', ln) for ln in lines) if m]
    check(dispatched == [4001, 4001, 5000, 324, 6139] and len(vtables) == 5, 'run 7066 fixture: dispatched %s, state-line vtables %s' % (dispatched, vtables))
    seen_75 = any('sequence 9 -> 75' in ln for ln in lines)
    seen_violation = any('read of guest 0x00002334' in ln for ln in lines)
    check(seen_75 and seen_violation, 'run 7066 fixture lacks the 9 -> 75 line or the access violation line')
    with tempfile.TemporaryDirectory() as tmp:
        exe = build_logic(rd('src', 'scene_stage_logic.h'), tmp, 'crash')
        ans = subprocess.run([exe], input=''.join('holds %d\n' % b for b in dispatched), capture_output=True, text=True, timeout=60).stdout.split()
    for b, vt, a in zip(dispatched, vtables, ans):
        # the story class (vtable 8200A410) held; 8200A4BC did not: HoldsInStage must say the same for every push of the run
        check((vt == '8200A410') == (a == 'HOLD'), 'run 7066: bank %d had cmd vtable %s but HoldsInStage says %s' % (b, vt, a))
    check(ans[-1] == 'ENDS' and dispatched[-1] == 6139 and vtables[-1] == '8200A4BC', 'run 7066: the push that crashed (6139) must be ENDS, the fixed play refuses it')
    print('7. run 7066 (the crash): of the 5 pushes, 4001 / 4001 / 5000 / 324 show the story class vtable 8200A410 and HoldsInStage says HOLD; 6139 shows 8200A4BC, then 9 -> 75 and the '
          'access violation at guest 0x2334, and HoldsInStage says ENDS (the fixed `play` refuses it before any guest write)')


def main():
    try:
        ctx = resolve.Context()
        addresses(ctx)
        parse_rule()
        logic()
        static_check()
        past_fact()
        wait_check()
        crash_check()
        import stage_leave_checks  # S101 lane 72: the arena, the leave verb, the runs 012 / 7072 / 7066 re-finds
        stage_leave_checks.run(check, rd, build_logic, resolve, address, stage_log, ss, ss, os.path.join(HERE, 'fixtures'))
    except RuntimeError as x:
        check(False, 'cannot read an input: %s' % x)
    if problems:
        print('FAILED: %d problem(s)' % len(problems))
        return 1
    print('stage selftest: all passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
