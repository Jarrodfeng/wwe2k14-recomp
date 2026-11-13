"""S101 lane 111 (go-anywhere L-2 and the small part of L-3): the harvest self test (gate lane moments_harvest). No game, no game data.

    py -I tools/goto/harvest_selftest.py

1. Carried place cvars: src/replay_carried.h (the code replay.h writes and reads a recording's header with) is COMPILED (clang++) and driven by
   tools/goto/replay_carried_test.cpp: a header written with the four place cvars reads back equal; an old recording without them still loads
   (CRLF too); wwe2k14_match_compare and a diagnosis option do not ride along.
2. E events: src/replay_verbs.h compiled and driven by tools/goto/replay_verbs_test.cpp: cvar batches, !wm30 and !scene become `E F text` lines
   at the frame that drains them; the text splits back for the playback handlers.
3. harvest.py on the fixture stream (tools/goto/fixtures/when_events_7060.jsonl): plan refuses hands, prints the flags; add indexes and prints
   the addressable moments with their frames; a recording without the place flags is refused.
4. Text pins (supplementary, not the proof): replay.h uses the shipped helpers; the three live verbs call Queue.
5. PLANTED TWINS: carried_dropped (the place cvar list loses wwe2k14_match_go), e_event_missing (Queue stores nothing), each written into a copy of
   the header and recompiled; and at the tool level a recording that lost its place flags. Each must fail its check.
Exit 0 = all passed.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
FIX = os.path.join(HERE, 'fixtures')
sys.path.insert(0, HERE)

problems = []


def check(ok, what):
    if not ok:
        problems.append(what)
        print('FAIL: ' + what)
    return ok


def find_clang():
    for c in (shutil.which('clang++'), r'C:\Program Files\LLVM\bin\clang++.exe', '/usr/bin/clang++'):
        if c and os.path.exists(c):
            return c
    raise RuntimeError("no clang++ found (tools\\env.ps1 puts the project's LLVM on PATH): the replay header unit tests cannot run")


def compile_test(cpp, inc, exe):
    r = subprocess.run([find_clang(), '-std=c++17', '-I', inc, os.path.join(HERE, cpp), '-o', exe], capture_output=True, text=True, timeout=240)
    if r.returncode != 0:
        raise RuntimeError('%s did not compile:\n%s' % (cpp, r.stderr[-1500:]))
    return exe


def run(exe, *args):
    r = subprocess.run([exe] + list(args), capture_output=True, text=True, timeout=60)
    return r.returncode, r.stdout


def mutant_inc(tmp, header, old, new, tag):
    """A copy of src/ with one text replacement in `header` (must hit exactly once): the planted fault."""
    d = os.path.join(tmp, 'inc_' + tag)
    os.makedirs(d)
    src = open(os.path.join(REPO, 'src', header), encoding='utf-8').read()
    assert src.count(old) == 1, 'planted fault text not found exactly once in %s: %r (%d)' % (header, old[:60], src.count(old))
    open(os.path.join(d, header), 'w', encoding='utf-8', newline='').write(src.replace(old, new))
    for other in ('replay_carried.h', 'replay_verbs.h'):   # the sibling the mutated header does not replace
        if other != header:
            shutil.copyfile(os.path.join(REPO, 'src', other), os.path.join(d, other))
    return d


PLACE_ROUNDTRIP_OK = 'OK 6'


def carried(tmp):
    src_inc = os.path.join(REPO, 'src')
    exe = compile_test('replay_carried_test.cpp', src_inc, os.path.join(tmp, 'carried.exe'))
    rc, out = run(exe, 'roundtrip')
    check(rc == 0 and out.strip() == PLACE_ROUNDTRIP_OK, 'CarriedSetting round trip (the four place cvars + det_frames survive, the others do not): %r' % out)
    hdr = os.path.join(tmp, 'new.rec')
    run(exe, 'write', hdr)
    text = open(hdr, encoding='utf-8').read()
    check('flag wwe2k14_match type=0;p=343:-1,324:-1;arena=4;pace=1\n' in text and 'flag wwe2k14_match_go 1\n' in text and 'flag wwe2k14_wm30 3\n' in text
          and 'flag wwe2k14_scene_victory 6343\n' in text and 'flag wwe2k14_wm30_auto false\n' in text,
          'the written header carries the four place cvars (and wm30_auto, which a preset recording may hold false) as flag lines')
    check('# modified: wwe2k14_match_compare = x' in text and '# modified: wwe2k14_launch_poke = 1' in text and '# modified: wwe2k14_fixed_seed' in text,
          'a non-place wwe2k14_match_* option and a diagnosis option stay comment lines')
    rc, out = run(exe, 'read', hdr)
    check(rc == 0 and 'flag wwe2k14_match_go = 1' in out and 'flag wwe2k14_scene_victory = 6343' in out, 'the header reads back: %r' % out)
    # an old recording (no place cvars), LF and CRLF, still loads
    old = '# wwe2k14 replay v2 (src/replay.h)\nname old\nseed 9\ndlc none\nflag det_frames true\nflag wwe2k14_launch 3\n# modified: x = y\nS 1 0 0 0 0 -\n'
    for nl, label in (('\n', 'LF'), ('\r\n', 'CRLF')):
        p = os.path.join(tmp, 'old_%s.rec' % label)
        open(p, 'w', newline='', encoding='utf-8').write(old.replace('\n', nl))
        rc, out = run(exe, 'read', p)
        check(rc == 0 and out.splitlines() == ['flag det_frames = true', 'flag wwe2k14_launch = 3'], 'an old %s recording without place flags still loads: %r' % (label, out))
    return exe


def carried_plant(tmp):
    """PLANT carried_dropped: the place cvar list loses wwe2k14_match_go -> the round trip must fail."""
    inc = mutant_inc(tmp, 'replay_carried.h', '"wwe2k14_match", "wwe2k14_match_go", "wwe2k14_wm30"', '"wwe2k14_match", "wwe2k14_wm30"', 'carried_dropped')
    exe = compile_test('replay_carried_test.cpp', inc, os.path.join(tmp, 'carried_mut.exe'))
    rc, out = run(exe, 'roundtrip')
    check(rc != 0 and 'missing wwe2k14_match_go' in out, 'PLANT carried_dropped is caught by the round trip (got %r)' % out)


EXPECT_VERBS = [
    # F6: nothing queued outside a recording
    'idle drained 0',
    # recording: host-only (vsync, screenshot dir, replay dir) and runin_now skipped (F3 / F4), a path value as <path>; !scene is not queued here (F1)
    'E 100 cvar wwe2k14_crowd_hype 80', 'E 100 cvar wwe2k14_match_go 7', 'E 100 wm30 3 X01', 'E 100 cvar some_game_path <path>', 'E 102 wm30 off',
    'host vsync 1', 'host window_width 1', 'host wwe2k14_screenshot_dir 1', 'host log_level 1', 'host gpu_extents_x 0', 'host gpu_native_detached 0',
    'host gpu_other 1', 'host vulkan_readback_fast 0', 'host vulkan_x 1', 'host wwe2k14_crowd_hype 0', 'host wwe2k14_gpu 1', 'host wwe2k14_gpu_x 0',
    'host audio_mute 1', 'host wwe2k14_match_go 0', 'host present_x 1',
    'split 1 [cvar] [wwe2k14_crowd_hype 80]', 'split 1 [wm30] [3 X01]', 'split 1 [scene] [victory 6343]', 'split 1 [wm30] [off]', 'split 1 [log] []',
    'playing 0', 'playing 1',
    # F1: served on the owner tick of 7 and 9, recorded there, replayed: served on the same ticks
    'live served 7 victory 6343', 'live served 9 menu 5', 'rec 7 scene victory 6343', 'rec 9 scene menu 5',
    'replay served 7 victory 6343', 'replay served 9 menu 5',
    # F2: two events in frame 20 are both served, in order, one per tick
    'replay served 20 mid 1,2', 'replay served 21 victory 9']


def verbs(tmp):
    exe = compile_test('replay_verbs_test.cpp', os.path.join(REPO, 'src'), os.path.join(tmp, 'verbs.exe'))
    rc, out = run(exe)
    check(rc == 0 and out.splitlines() == EXPECT_VERBS, 'live verbs: E lines at the draining frame, host-only / runin_now skipped, idle queue empty, scene recorded at the serve '
          'frame and served on that frame on playback, two in one frame both served: %r' % out)
    # HostOnly parity with HOST_ONLY in tools/determinism/det_cross.py (the C++ port must give the regex's answer for these names)
    import re
    src = open(os.path.join(REPO, 'tools', 'determinism', 'det_cross.py'), encoding='utf-8').read()
    import ast
    tree = ast.parse(src)
    node = next(n for n in tree.body if isinstance(n, ast.Assign) and getattr(n.targets[0], 'id', '') == 'HOST_ONLY')
    rx = re.compile(ast.literal_eval(node.value.args[0]))   # the file's own pattern text
    for line in out.splitlines():
        if line.startswith('host '):
            _, name, got = line.split()
            check(bool(rx.match(name)) == bool(int(got)), 'HostOnly(%s) = %s agrees with det_cross.py HOST_ONLY' % (name, got))
    return exe


PLANTS = [   # (tag, header, old text (exactly once), new text, what must differ)
    ('e_event_missing', 'replay_verbs.h', '  g_queue.push_back(text);', '  (void)text;', 'E 100 wm30 3 X01'),
    ('e_scene_drain_frame', 'replay_verbs.h', 'g_record_event(tick_frame, SceneText(scene_args))', 'g_record_event(tick_frame + 1, SceneText(scene_args))', 'replay served 7 victory 6343'),
    ('scene_fifo_overwrite', 'replay_verbs.h', '  g_scene_replay.push_back(args);', '  g_scene_replay.clear();\n  g_scene_replay.push_back(args);', 'replay served 20 mid 1,2'),
    ('host_cvar_recorded', 'replay_verbs.h', '  if (HostOnly(name) || name == "wwe2k14_runin_now") return;', '  if (false) return;', 'host vsync 1'),
    ('runin_now_recorded', 'replay_verbs.h', ' || name == "wwe2k14_runin_now") return;', ') return;', 'E 100 cvar wwe2k14_runin_now 1'),
    ('idle_queue_grows', 'replay_verbs.h', '  if (!Recording()) return;\n  std::lock_guard', '  std::lock_guard', 'idle drained 0'),
]


def verbs_plants(tmp):
    """Each planted fault is written into a copy of the shipped header, recompiled, and must change the test's output (so the check above fails)."""
    for tag, header, old, new, must_change in PLANTS:
        inc = mutant_inc(tmp, header, old, new, tag)
        exe = compile_test('replay_verbs_test.cpp', inc, os.path.join(tmp, 'verbs_%s.exe' % tag))
        rc, out = run(exe)
        lines = out.splitlines()
        # the mutated output must differ from the expected output, and in the line(s) the fault is about
        bad = lines != EXPECT_VERBS
        if tag == 'runin_now_recorded':
            bad = bad and 'E 100 cvar wwe2k14_runin_now 1' in lines
        elif tag == 'e_event_missing':
            bad = bad and 'E 100 wm30 3 X01' not in lines
        check(bad, 'PLANT %s is caught (the test output differs from the expected one)' % tag)


def text_pins():
    rep = open(os.path.join(REPO, 'src', 'replay.h'), encoding='utf-8').read()
    check('#include "replay_carried.h"' in rep and 'HeaderLine(name, value, host_path)' in rep and 'ParseFlagLine(line, kv)' in rep and
          'for (const std::string& t : verbs::Drain()) RecordEvent(frame, t);' in rep, 'replay.h writes / reads its header with the shipped helpers and drains the verb queue into RecordEvent')
    check('static const char* kPrefixes' not in rep, 'replay.h no longer holds a second copy of the carried list')
    check('std::atomic<bool> playing_{false};' in rep, 'replay.h: playing_ is atomic (the live channel reads it from the pipe thread)')
    inp = open(os.path.join(REPO, 'src', 'input_script.h'), encoding='utf-8').read()
    check('replay::verbs::QueueCvar(step.text, step.value)' in inp, 'input_script.h ApplyCvar queues the cvar E event')
    lc = open(os.path.join(REPO, 'src', 'live_control.h'), encoding='utf-8').read()
    check('replay::verbs::Queue(replay::verbs::Wm30Text(wm30_args))' in lc and 'replay::verbs::Trim(wm30_args) == "off"' in lc and
          'replay::verbs::Trim(scene_args) != "off"' in lc, 'live_control.h queues !wm30, lets the off forms through while playing')
    sd = open(os.path.join(REPO, 'src', 'scene_director.h'), encoding='utf-8').read()
    check('replay::verbs::RecordServed(Frame(), request_text)' in sd and 'replay::verbs::SceneReplayTake(args)' in sd and 'replay::verbs::SceneReplayPush(rest)' in sd,
          'scene_director.h records at the serve site and takes one replayed request per tick')
    wm = open(os.path.join(REPO, 'src', 'wm30_jump.h'), encoding='utf-8').read()
    check('g_event_handlers.push_back(&OnReplayEvent)' in wm and 'g_event_handlers.push_back(&OnReplayEvent)' in sd, 'wm30 and scene have their playback handlers')


def harvest_tool(tmp):
    import harvest as H
    import moments
    lib = os.path.join(tmp, 'lib')
    os.makedirs(lib)
    os.environ['RDR_MOMENTS_DIR'] = lib

    def refused(spec):
        try:
            H.canonical_place(spec)
        except H.HarvestError as x:
            return str(x)
        return None

    check('no hands' in (refused('Daniel Bryan vs Brock Lesnar; players=1') or ''), 'a spec with players=1 is refused ("a harvest has no hands")')
    check('no hands' in (refused('Daniel Bryan vs Brock Lesnar') or ''), 'a spec with no players= (default 1) is refused')
    check('no hands' in (refused('type=0;p=343:0,324:-1;arena=4;pace=1') or ''), 'a raw spec with a pad >= 0 is refused')
    raw = 'type=0;p=343:-1,324:-1;arena=4'
    place, numeric = H.canonical_place(raw)
    check(numeric == 'type=0;p=343:-1,324:-1;arena=4;pace=1' and place.endswith('pace=1'), 'a raw CPU spec gets pace=quick (pace=1) added: %s' % numeric)
    out = os.path.join(tmp, 'plan_out')
    p = H.plan(raw, slug='harvest_fx', out=out, quiet=True)
    fl = p['flags']
    want = ['--wwe2k14_replay_record=harvest_fx', '--wwe2k14_match=' + numeric, '--wwe2k14_match_go=1', '--wwe2k14_events=true',
            '--wwe2k14_det_hash=default,objects,sim', '--wwe2k14_det_hash_v2=true']
    check(fl == want, 'plan flags: record, events, det hash, the match: %s' % fl)
    # F7: the printed run command survives a shell split (the match spec holds ';')
    import shlex
    import re as _re
    def shsplit(text):   # a shell split that treats an unquoted ';' as a command separator (punctuation_chars), like PowerShell / sh do
        lx = shlex.shlex(text, posix=True, punctuation_chars=True)
        lx.whitespace_split = True
        return list(lx)
    toks = shsplit(p['run'])
    check([t for t in toks if t.startswith('--wwe2k14_')] == want, 'the printed run command splits back into every flag (POSIX split): %s' % toks)
    ps = [m.group(1) if m.group(1) is not None else m.group(2) for m in _re.finditer(r'"((?:[^"\\]|\\.)*)"|(\S+)', p['run'])]   # PowerShell-style: "..." is one argument
    check([t for t in ps if t.startswith('--wwe2k14_')] == want, 'the printed run command splits back into every flag (PowerShell-style split): %s' % ps)
    check(';' in numeric and not any(';' in t and not t.startswith('--wwe2k14_match=') and not t.endswith('route.txt') for t in toks), 'only the match flag holds a ";" and it is one token')
    unq = 'py -I tools\\run\\run.py "x" --check ' + ' '.join(fl)
    check([t for t in shsplit(unq) if t.startswith('--wwe2k14_')] != want, 'PLANT unquoted_flags: the old unquoted command is cut at the ";" (twin of the check above)')
    route = open(p['route'], encoding='utf-8').read()
    check('include boot_to_menu' in route and 'wait_state seq 9' in route and 'quit' in route and 'set 0x' not in route and 'press' not in route,
          'the route only boots to the menu and waits (no pad input in the match)')
    check(json.load(open(os.path.join(out, 'plan.json')))['place'] == place, 'plan.json holds the place the library compares')
    # the text spec path (needs the atlas data of this checkout; refusal order is checked above without it)
    try:
        t = H.plan('Daniel Bryan vs Brock Lesnar; players=0', slug='harvest_txt', out=os.path.join(tmp, 'plan_txt'), quiet=True)
        check(t['numeric'].startswith('type=0;p=') and ':-1' in t['numeric'] and t['numeric'].endswith('pace=1') and t['place'].endswith('pace=quick'),
              'a text spec builds the numeric spec with every seat CPU: %s' % t['numeric'])
    except H.HarvestError as x:
        print('note: text spec path skipped (%s)' % x)

    # add: a recording that carries the place flags and the fixture bus stream
    def rec_text(flags, last=20280):
        return '# wwe2k14 replay v2 (src/replay.h): synthetic fixture of tools/goto/harvest_selftest.py\nname fx\nseed 77\ndlc none\n' + flags + \
            ''.join('T %d %d\n' % (f, f * 3) for f in range(60, last + 1, 60))
    good = os.path.join(tmp, 'good.rec')
    open(good, 'w', newline='').write(rec_text('flag wwe2k14_match %s\nflag wwe2k14_match_go 1\nflag det_frames true\n' % numeric))
    bad = os.path.join(tmp, 'bad.rec')
    open(bad, 'w', newline='').write(rec_text('flag det_frames true\n'))   # PLANT carried_dropped at the tool level: the recording lost its place flags
    ev = os.path.join(FIX, 'when_events_7060.jsonl')
    try:
        H.add('harvest_fx', bad, ev, out=out, lib=lib)
        check(False, 'PLANT carried_dropped (recording without place flags) must be refused by add')
    except H.HarvestError as x:
        check('wwe2k14_match' in str(x) and 'rebuild the match' in str(x), 'PLANT carried_dropped is caught by add: %s' % x)
    try:
        H.add('harvest_fx', good, '', out=out, lib=lib)
        check(False, 'add without the bus stream must be refused')
    except H.HarvestError as x:
        check('bus stream' in str(x), 'add without --events is refused: %s' % x)
    meta, addrs = H.add('harvest_fx', good, ev, out=out, lib=lib)
    got = {a: f for a, f, _ in addrs}
    exp = {'moment:harvest_fx@pin': 15081, 'moment:harvest_fx@pin@count:1': 15110, 'moment:harvest_fx@pin@count:2': 15168, 'moment:harvest_fx@finish': 15227}
    check(got == exp, 'addressable moments of the 7060 stream (pin 15081, counts 15110 / 15168, finish 15227): %s' % got)
    check(meta['place'] == place and not moments.check_moment('harvest_fx', lib), 'the moment carries the place and checks clean')
    hits, _ = moments.find_moments('@pin', place=place, lib=lib)
    check(hits and hits[0][0] == 15081, 'find @pin --place finds the harvest by its place')
    # a synthetic stream with kickouts and eliminations
    syn = os.path.join(tmp, 'syn.jsonl')
    lines = [{'f': 1, 'kind': 'seq', 'seq': 9, 'from': 3}, {'f': 500, 'kind': 'match', 'what': 'clock_started', 'clock': 1},
             {'f': 900, 'kind': 'match', 'what': 'pin', 'pinning_slot': 0}, {'f': 929, 'kind': 'match', 'what': 'count', 'count': 1},
             {'f': 987, 'kind': 'match', 'what': 'count', 'count': 2}, {'f': 1007, 'kind': 'match', 'what': 'kickout', 'at_count': 2, 'stopped_f': 987},
             {'f': 2000, 'kind': 'match', 'what': 'pin', 'pinning_slot': 1}, {'f': 2029, 'kind': 'match', 'what': 'count', 'count': 1},
             {'f': 2200, 'kind': 'match', 'what': 'eliminated', 'slot': 2, 'id': 101}, {'f': 2500, 'kind': 'match', 'what': 'eliminated', 'slot': 3, 'id': 102},
             {'f': 3000, 'kind': 'match', 'what': 'finish', 'finish_type': 1, 'finish_name': 'pinfall', 'winner_slot': 1}]
    open(syn, 'w').write(''.join(json.dumps(l) + '\n' for l in lines))
    open(good, 'w', newline='').write(rec_text('flag wwe2k14_match %s\nflag wwe2k14_match_go 1\n' % numeric, 3060))
    H.plan(raw, slug='harvest_syn', out=out, quiet=True)
    meta2, addrs2 = H.add('harvest_syn', good, syn, out=out, lib=lib)
    got2 = {a.replace('moment:harvest_syn', ''): f for a, f, _ in addrs2}
    exp2 = {'@pin': 900, '@pin@count:1': 929, '@pin@count:2': 987, '@kickout': 1007, '@pin#2': 2000, '@pin#2@count:1': 2029, '@eliminated#1': 2200,
            '@eliminated#2': 2500, '@finish': 3000}
    check(got2 == exp2, 'every pin, count, kickout, elimination and finish of a synthetic stream is addressed with its frame: %s' % got2)
    check(all(f is not None for f in got2.values()), 'every printed address resolves')


def main():
    tmp = tempfile.mkdtemp(prefix='l111_harvest_')
    try:
        carried(tmp)
        carried_plant(tmp)
        verbs(tmp)
        verbs_plants(tmp)
        text_pins()
        harvest_tool(tmp)
    except RuntimeError as x:
        problems.append(str(x))
        print('FAIL: ' + str(x))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if problems:
        print('%d problem(s)' % len(problems))
        return 1
    print('harvest self test: all checks passed (0 problems; planted carried_dropped and e_event_missing both caught)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
