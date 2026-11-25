r"""S110 SCENETOOL: the scene tool END TO END with no game: tools\scene\go.py on the REAL catalogue.py (reading a synthetic catalogue file in its
contract's format), the REAL tools\run\scene.py (lowering, lint, describe) and the REAL tools\bench\run_read.py; only run_list.ps1 is a stand-in.

    py -I tools\scene\go_selftest.py        exit 0 = every check passed

The run_list stand-in does what run_list's scene door does (tools\bench\run_list_scene.ps1): it lowers the `scene:` request with the real scene.py
--write on the exe it was given (a stub exe file holding the strings an exe build would hold: with or without lane CASARRIVE's `cas_record`), prints
run_list's lines (`-Exe tip -> EXE`, `scene request -> ROUTE`, the result line) and writes a synthetic run log that prints the route's own
`# expect:` lines (all, all but one, or a FAILED wait), so the hand-back is judged on what the lowering really asked for. It refuses to run when the
question is not in runs.md yet. Earlier stand-ins of the catalogue and the lowering hid a contract break (go.py read `arrival` as a string):
those two are never stubbed here.
"""
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
GO = os.path.join(HERE, 'go.py')
sys.path.insert(0, HERE)
import catalogue  # noqa: E402
import mode_table  # noqa: E402
import known_noise  # noqa: E402

# the catalogue's contract (tools\scene\catalogue.py): KIND<TAB>ID<TAB>NAME<TAB>ALIASES<TAB>WHERE; ids of the real db\ids (scene.py resolves them)
ROWS = [('wrestler', '324', 'Daniel Bryan', '', 'WHO #324 (scene NAME vs ...; or #324)'),
        ('wrestler', '343', 'Brock Lesnar', '', 'WHO #343 (scene NAME vs ...; or #343)'),
        ('wrestler', '161', 'Randy Orton', '', 'WHO #161 (scene NAME vs ...; or #161)'),
        ('wrestler', '205', 'Cody Rhodes', '', 'WHO #205 (scene NAME vs ...; or #205)'),
        ('wrestler', '388', 'Dusty Rhodes', 'dlc pack 5', 'WHO #388 ; dlc=on'),
        ('arena', '15', 'WrestleMania 29', 'wm29 | wm 29 | wrestlemania 29', 'arena=15'),
        ('wm30', '15', 'Kilo Lima vs. Mike November', 'Planted Event 12', 'scene wm30 15'),   # made-up names (rule 1: no game text here)
        ('cas', '76', 'CLOTHING > UPPER BODY > TOPS', 'TOPS', 'scene cas new // at page=76 -> screen 1002'),
        ('cas', '77', 'CLOTHING > UPPER BODY > TOPS (2)', 'TOPS (2)', 'scene cas new // at page=77 -> screen 1002'),
        ('menu', '7', 'OPTIONS', '', 'scene menu // at page=7 -> list:4'),
        ('menu', 'm4.3', 'OPTIONS > SOUND', 'SOUND', 'scene menu // at page=? (record id unknown: no menu listing read) -> ?'),
        ('moment', 'bell', 'bell', 'match / clock_started', 'at bell : ITEMS (in-route state)')]

RUNNER = r'''
import os, subprocess, sys, time
a = sys.argv[1:]
req, exe = a[a.index('--request') + 1], a[a.index('--exe') + 1]
mode = os.environ['SCENE_GO_STUB_MODE']
runs = os.path.join(os.environ['SCENE_GO_OUT'], 'runs', 'runs.md')
if not (os.path.isfile(runs) and req[len('scene:'):].strip() in open(runs, encoding='utf-8').read()):
    print('ABORT: stand-in: the question was not in runs.md before the run')
    sys.exit(2)
if mode == 'hang':
    time.sleep(20)
print('[run_list] -Exe tip -> %s' % exe)
p = subprocess.run([sys.executable, '-I', os.environ['SCENE_GO_STUB_LOWER'], '--write', req, '--exe', exe, '--out', os.environ['SCENE_GO_STUB_SCENES']],
                   capture_output=True, text=True)
last = (p.stdout.strip().splitlines() or [''])[-1]
if p.returncode != 0 or not last.endswith('.txt'):
    print('ABORT: scene request refused: %s' % last)
    sys.exit(2)
route = last
print('[run_list] scene request -> %s' % route)
expect = [ln[len('# expect: '):] for ln in open(route, encoding='utf-8').read().splitlines() if ln.startswith('# expect: ')]
log_dir = os.path.join(os.path.dirname(exe), 'logs')
cap = os.path.join(os.path.dirname(exe), 'captures')
os.makedirs(log_dir, exist_ok=True)
os.makedirs(cap, exist_ok=True)
H = '[2026-11-25 09:30:%02d.000] [%s] [rex] [t7] '
lines = [H % (0, 'info') + '[wwe2k14] run log number 99042']
# the boot's known error groups (tools\scene\known_noise.py), each closed by an info line so run_read groups them as a clean run shows them
for msg in (['BaseHeap::Alloc failed to find contiguous range'], ['Memory Dump', '00000000: 0A 0B 0C 0D', '00000010: 0E 0F 10 11'],
            ['ResolvePath(UPDATE:' + chr(92) + ') failed - device not found'], ['NtQueryInformationFile(XFileXctdCompressionInformation) unimplemented']):
    lines += [H % (0, 'error') + m for m in msg] + [H % (0, 'info') + '[wwe2k14] boot']
if mode == 'newerr':
    lines += [H % (1, 'error') + 'XAudio2 voice 3 lost its buffer', H % (1, 'info') + '[wwe2k14] going on']
if mode == 'not':
    lines += [H % (1, 'error') + '[wwe2k14][input] script FAILED at line 9: wait_state seq 77 3600 2 not reached within 3600 frames (last: 1; frame 3700)']
elif mode == 'ended':   # S110 SCENEMOM2: the until clause ended the wait (src/input_script.h:2467's line; longer than run_read's 300-character text)
    lines += [H % (1, 'error') + '[wwe2k14][input] script FAILED at line 14: wait_event match what=finish finish_name=pinfall until match what=finish: '
              'ended by match what=finish after 5310 frames (seq 377, bus frame 9020, frame 9031): "what": "finish", "finish_type": 6, '
              '"finish_name": "submission", "winner_slot": 0, "winner_id": 324, "loser_slot": 1, "loser_id": 343, "clock": 2675, "table_rule": 0, '
              '"count_out_rule": 0']
elif mode == 'tablebreak':   # S110 SCENEMOM2 review: the bus's own line of a table break (src\event_bus.h Emit, src\table_break_core.h Fields), ~230 characters
    lines += [H % (i + 1, 'info') + '[wwe2k14][event] {"f": 14676, "t": 245.112, %s, "obj": "0x4A123456", "act": 15380, "previous": 15379, "track": 0, '
              '"weapon_kind": 5, "on_fire": 0, "via": "hit", "body_slot": 1, "body_id": 343}' % e for i, e in enumerate(expect)]
    lines += [H % (40, 'info') + '[wwe2k14][input] line 12: bench: done (frame 4400)']
elif mode == 'falls':   # S110 TOOLFIT: an Iron Man's falls carry the finish's record (S110 ELIMFIN) before the finish at the time limit (run 299)
    rec = '"finish_type": 1, "finish_name": "pinfall", "winner_slot": 0, "winner_id": 139, "loser_slot": 1, "loser_id": 161, "clock": %d, "table_rule": 0, "count_out_rule": 0'
    lines += [H % (1, 'info') + '[wwe2k14][event] {"f": 22741, "t": 379.017, "kind": "match", "what": "fall", "scorer_slot": 0, "scorer_id": 139, "falls": 1, ' + rec % 13400 + '}',
              H % (2, 'info') + '[wwe2k14][event] {"f": 72217, "t": 1203.617, "kind": "match", "what": "finish", ' + rec % 36000 + ', "winner_falls": 3, "loser_falls": 1}',
              H % (40, 'info') + '[wwe2k14][input] line 12: bench: done (frame 72300)']
elif mode == 'fitfamily':   # S110 TOOLFIT: the hold wish met only by a posture-family hold: the assert fails after the bell
    lines += [H % (1, 'info') + '[wwe2k14][director] director: finisher 0 hold (slots 17 / 18 = 50326 / 58350: a hold finisher fits only in a posture family '
              '(50326 not a hold; 58350 a hold in posture family 58350..58398 (sub_82602DE0 +1CC0, r21 5)): no family-free hold (ids 58000..58099) to keep; '
              'char 0x4A100000) clock 2',
              H % (2, 'error') + '[wwe2k14][input] script FAILED at line 16: assert_state finisher_fit_0 1 (is 0x00000002 (2) [focus], frame 3100)']
else:
    keep = expect if mode in ('arrived', 'newerr') else expect[:-1]
    lines += [H % (i + 1, 'info') + '%s (stand-in line) (frame %d)' % (e, 4000 + 100 * i) for i, e in enumerate(keep)]
    lines += [H % (40, 'info') + '[wwe2k14][input] line 12: bench: done (frame 4400)']
with open(os.path.join(log_dir, 'wwe2k14_99042.log'), 'w', encoding='utf-8') as fh:
    fh.write('\n'.join(lines) + '\n')
for n in ('99042_arrived.png', '99042__sheet.jpg'):
    open(os.path.join(cap, n), 'wb').write(b'x')
print('%s -> wwe2k14_99042  EXITED after 8.5s, exit code 0 (0x00000000)  ' % os.path.basename(route))
'''

FAILS = []


def check(ok, what, detail=''):
    print('%s  %s' % ('ok  ' if ok else 'FAIL', what))
    if not ok:
        FAILS.append(what)
        if detail:
            print('      ' + detail.replace('\n', '\n      '))


def stub_exe(folder, state, extra=()):
    """An exe file holding the strings route_lint.exe_knows looks for: every flag the lowerings use, the @cas / @mode: launches, the `page` line;
    lane CASARRIVE's `cas_record` only when `state`; `extra` more (S110 TOOLFIT: the finisher verb's probe). One folder each (exe_knows caches by folder)."""
    os.makedirs(folder, exist_ok=True)
    names = ['wwe2k14_fixed_seed', 'vulkan_stored_pipelines_at_boot', 'wwe2k14_events', 'wwe2k14_rdr_owner', 'audio_mute', 'wwe2k14_launch',
             'wwe2k14_launch_participants', 'wwe2k14_launch_arena', 'wwe2k14_dlc', 'wwe2k14_cas_goto', '@cas', '@mode:', 'wwe2k14_wm30',
             'wwe2k14_wm30_direct', 'page needs a record id: page N [back]', 'wwe2k14_quick_type'] + (['cas_record', 'cas_target'] if state else []) + list(extra)
    p = os.path.join(folder, 'wwe2k14.exe')
    with open(p, 'wb') as fh:
        fh.write(b'MZ\0' + b'\0'.join(n.encode() for n in names) + b'\0')
    return p


def main():
    tmp = tempfile.mkdtemp(prefix='scene_go_selftest_')
    cat = os.path.join(tmp, 'catalogue.txt')
    modes = catalogue.Build(ROOT, tmp, tmp)   # the mode lines exactly as `catalogue.py build` writes them (src\mode_table.h through mode_table.py)
    modes.modes()
    with open(cat, 'w', encoding='utf-8', newline='\n') as fh:
        fh.write('# a synthetic catalogue (go_selftest.py)\n' + ''.join('\t'.join(r) + '\n' for r in ROWS) + ''.join(catalogue.line_of(e) + '\n' for e in modes.entries))
    scenes = os.path.join(ROOT, 'out', 'scene', 'go_selftest_%d' % os.getpid())   # scene.py --write takes a folder under out\ only (as run_list's door writes out\scenes)
    runner = os.path.join(tmp, 'run_list_stub.py')
    with open(runner, 'w', encoding='utf-8') as fh:
        fh.write(RUNNER)
    exe_new, exe_old, exe_nomode = stub_exe(os.path.join(tmp, 'exe_new'), True), stub_exe(os.path.join(tmp, 'exe_old'), False), os.path.join(tmp, 'exe_nomode', 'wwe2k14.exe')
    exe_fin = stub_exe(os.path.join(tmp, 'exe_fin'), True, ['[wwe2k14][finisher]'])   # S110 TOOLFIT: an exe with the `finisher` verb (scene_moments FIT_PROBE)
    os.makedirs(os.path.dirname(exe_nomode))
    with open(exe_nomode, 'wb') as fh:
        fh.write(b'MZ\0wwe2k14_launch\0')
    out_scene = os.path.join(tmp, 'out_scene')
    env = dict(os.environ, SCENE_GO_CATALOGUE_FILE=cat, SCENE_GO_RUNNER=runner, SCENE_GO_OUT=out_scene,
               SCENE_GO_STUB_LOWER=os.path.join(ROOT, 'tools', 'run', 'scene.py'), SCENE_GO_STUB_SCENES=scenes)

    def go(*args, mode='arrived'):
        p = subprocess.run([sys.executable, '-I', GO] + list(args), capture_output=True, text=True, encoding='utf-8', env=dict(env, SCENE_GO_STUB_MODE=mode),
                           timeout=300)
        return p.returncode, p.stdout + p.stderr

    def runs_lines():
        p = os.path.join(out_scene, 'runs', 'runs.md')
        return open(p, encoding='utf-8').read().splitlines() if os.path.isfile(p) else []
    try:
        # 1. --print: the real catalogue resolves, the real lowering's JSON is read (arrival {kind, line, expect[]}), nothing starts
        c, o = go('cas new at TOPS', '--print', '--exe', exe_old)
        check(c == 0 and 'SCENE    scene cas new // at page=76 : screenshot arrived\n' in o and 'ARRIVAL  frames: wait_frames 1800\n' in o
              and 'EXPECT   [wwe2k14][launch] @cas: sequence 76 is current at frame\n' in o and 'EXPECT   [wwe2k14][cas_goto] record 76: arrived at record 76\n' in o
              and o.rstrip().endswith('EXIT     0 printed (nothing started)'), 'cas new at TOPS -> page=76; an exe without cas_record: the frame count + 2 expected lines', o)
        c, o = go('cas new at TOPS', '--print', '--exe', exe_new)
        check(c == 0 and 'ARRIVAL  state: wait_state cas_record 76 3600\n' in o and 'EXPECT   [wwe2k14][cas_goto] state cas_goto=2 cas_record=76 \n' in o,
              'an exe with lane CASARRIVE\'s state: wait_state cas_record 76 (the record), its state line expected', o)
        check(not os.path.exists(os.path.join(out_scene, 'runs')), '--print writes nothing (no runs folder, no runs.md)')
        c, o = go('Daniel Bryan vs Brock Lesnar in WrestleMania 29 at bell', '--print', '--exe', exe_new)
        check(c == 0 and 'SCENE    scene #324 vs #343; arena=15 // at bell : screenshot arrived\n' in o and 'ARRIVAL  state: wait_state match_clock >0 30000\n' in o,
              'a match by names at the bell: wrestlers -> #ids, arena -> its id', o)
        c, o = go('mode threads', '--print', '--exe', exe_new)
        row = mode_table.find('threads')
        check(c == 0 and 'SCENE    scene mode threads // at arrived : screenshot arrived\n' in o and all('EXPECT   %s\n' % e in o for e in mode_table.expect_lines(row))
              and 'ARRIVAL  state: wait_state seq %d 3600 2\n' % row['seq'] in o, 'mode threads: src\\mode_table.h\'s row, its three launch lines expected', o)
        c, o = go('Daniel Bryan vs Brock Lesnar : screenshot x', '--print', '--exe', exe_new)
        c2, o2 = go('mode threads', '--print', '--exe', exe_new)
        check(c == 0 and 'SCENE    scene #324 vs #343 // at bell : screenshot x\n' in o and c2 == 0 and 'SCENE    scene mode threads // at arrived : screenshot arrived\n' in o2,
              'no `at`: the mode\'s own arrival position (a match: bell, never `arrived`, which scene.py refuses for a match); no items: a screenshot', o + o2)
        c, o = go('mode create a superstar', '--print', '--exe', exe_new)
        check(c == 0 and 'SCENE    scene cas new // at editor : screenshot arrived\n' in o, 'the cas row of the mode table is `cas new` (one way in)', o)
        c, o = go('wm30 Kilo Lima vs Mike November', '--print', '--exe', exe_new)
        check(c == 0 and 'SCENE    scene wm30 15 // at bell : screenshot arrived\n' in o and 'EXPECT   [wwe2k14][wm30][direct] exit action 3 requested\n' in o,
              'a 30 Years match by its names', o)
        c, o = go('menu at OPTIONS', '--print', '--exe', exe_new)
        check(c == 0 and 'SCENE    scene menu // at page=7 : screenshot arrived\n' in o and 'EXPECT   [wwe2k14][menu_goto] record 7 page change done\n' in o, 'a front-end page by name', o)
        c, o = go('Dusty Rhodes vs Randy Orton', '--print', '--exe', exe_new)
        check(c == 0 and 'SCENE    scene #388 vs #161; dlc=on // at bell : screenshot arrived\n' in o and '--wwe2k14_dlc=true' in o, 'a DLC wrestler adds dlc=on by itself', o)
        # 2. refusals: ambiguous, unknown, a page with no record id, a bad --data; each exit 2, nothing written
        c, o = go('Rhodes vs Randy Orton', '--print')
        check(c == 2 and '"Rhodes" (wrestler): ambiguous, 2 match; say which: Cody Rhodes (205) | Dusty Rhodes (388)' in o and 'EXIT     2 refused' in o,
              'an ambiguous name is refused with its candidates', o)
        c, o = go('Zqxv Wplk vs Randy Orton', '--print')
        check(c == 2 and '"Zqxv Wplk" (wrestler): unknown; nearest: ' in o and 'SCENE    (not resolved) Zqxv Wplk vs Randy Orton' in o, 'an unknown name is refused with the nearest names', o)
        c, o = go('menu at SOUND', '--print')
        check(c == 2 and 'has no record id in the catalogue' in o and 'catalogue.py check' in o, 'a page the catalogue has no record id for is refused with the way to make it', o)
        c, o = go('cas new at TOPS', '--data', '..\\..\\AppData')
        check(c == 2 and 'a mirror NAME' in o and not runs_lines(), '--data takes a mirror name, never a path (no run, nothing logged)', o)
        c, o = go('Daniel Bryan vs Brock Lesnar at finish=pinfall : screenshot fin', '--print', '--exe', exe_new)
        check(c == 0 and 'SCENE    scene #324 vs #343 // at finish=pinfall : screenshot fin\n' in o
              and 'EXPECT   "what": "finish", "finish_type": 1, "finish_name": "pinfall", "winner_slot": \n' in o
              and 'ARRIVAL  state: wait_event finish finish_name=pinfall 72000 until finish\n' in o and '# recipe: momentum 0 100, stock 0 1, passive 1 on' in o,
              'a bus moment by its words: the wait_event, its bus line expected, the default recipe in the header (S110 SCENEMOMENTS)', o)
        c, o = go('cas new at TOPS : screenshot a at TOPS (2) at editor', '--print', '--exe', exe_new)
        check(c == 2 and 'editor' in o, 'several ` at `: the lowering\'s own refusal comes back (a page and `editor` in one request)', o)
        c, o = go('cas new at TOPS : screenshot a at TOPS (2)', '--print', '--exe', exe_new)
        check(c == 0 and 'SCENE    scene cas new // at page=76 : screenshot a // at page=77 : screenshot arrived_2\n' in o
              and 'ARRIVAL  state: assert_state cas_record 77\n' in o and 'cvar wwe2k14_cas_goto 77' in o,
              'several ` at `: two CAS pages in one request, each by cvar (S110 SCENEMOMENTS)', o)
        c, o = go('cas new at SLEEVES', '--exe', exe_new)
        check(c == 2 and '"SLEEVES" (cas): unknown; nearest: ' in o and not os.path.exists(os.path.join(out_scene, 'runs')),
              'an unknown CAS page is refused before anything is written or run', o)
        # 3. runs: the question first, then the route the door lowered on the exe it ran, read with EVERY expected line
        c, o = go('cas new at TOPS', '--exe', exe_new)
        got = o.strip().splitlines()
        rl = runs_lines()
        check(c == 0 and got[0] == 'SCENE    scene cas new // at page=76 : screenshot arrived' and got[1].startswith('ARRIVED  yes (frame 4200, line 14: [wwe2k14][cas_goto] state cas_goto=2 cas_record=76')
              and got[-1] == 'EXIT     0 arrived' and len(got) == 6, 'an arrived CAS run (state exe): yes on the LAST of its 3 expected lines (after 10 boot-noise lines), exit 0', o)
        check(got[4].startswith('LOG      PROBLEMS 0 | errors: known noise only (4 groups) | SHORTFALLS '),
              'a clean run: the LOG line says known noise only (the 4 boot groups, tools\\scene\\known_noise.py)', o)
        check(len(rl) == 1 and 'request `cas new at TOPS`' in rl[0] and '`scene cas new // at page=76 : screenshot arrived`' in rl[0],
              'runs.md holds the question, written before the run (the stand-in refuses otherwise)', '\n'.join(rl))
        c, o = go('cas new at TOPS', '--exe', exe_old, mode='partial')
        check(c == 1 and 'ARRIVED  NO (the route finished, but 1 expected game line(s) never printed: "[wwe2k14][cas_goto] record 76: arrived at record 76")' in o
              and 'EXIT     1 not arrived' in o, 'bench: done alone is not arrival: a missing expected line (the frame-count fallback) is NO, exit 1', o)
        c, o = go('mode threads', '--exe', exe_new, mode='not')
        check(c == 1 and 'ARRIVED  NO (frame 3700: [wwe2k14][input] script FAILED at line 9' in o and 'never printed' in o, 'a failed wait: the failing line and the lines never printed, exit 1', o)
        c, o = go('wm30 15', '--exe', exe_new)
        check(c == 0 and 'ARRIVED  yes (frame 4000, line 12: [wwe2k14][wm30][direct] exit action 3 requested' in o, 'an arrived 30 Years match', o)
        c, o = go('Daniel Bryan vs Brock Lesnar in WrestleMania 29 at bell', '--exe', exe_new)
        check(c == 0 and 'ARRIVED  yes (frame 4400, line 12: [wwe2k14][input] line 12: bench: done' in o, 'a match (no expected lines): arrival is `bench: done` past the bell wait', o)
        c, o = go('mode threads', '--exe', exe_new, mode='newerr')
        log_line = next((ln for ln in o.splitlines() if ln.startswith('LOG ')), '')
        check(c == 0 and 'NEW error groups 1: "XAudio2 voice 3 lost its buffer" x1 from line ' in log_line and '(+4 known noise)' in log_line,
              'a new error group is NAMED on the LOG line beside the known noise (error groups do not decide the exit)', o)
        rp = 'ResolvePath(<path>) failed - device not found'   # review: any path normalizes to <path>; a clean run prints 3 (UPDATE:\ once, \Device twice)
        check(known_noise.new_groups([{'key': rp, 'count': 3}]) == [] and len(known_noise.new_groups([{'key': rp, 'count': 4}])) == 1,
              'known noise: a 4th device-not-found path is NEW, the boot\'s 3 are not (tools\\scene\\known_noise.py)')
        c, o = go('mode threads', '--exe', exe_nomode)
        check(c == 2 and 'REFUSED' in o and '@mode:' in o, 'the door\'s lowering on the run\'s own exe refuses (an exe without @mode:): exit 2 with its words', o)
        c, o = go('mode threads', '--exe', exe_new, '--timeout', '4', mode='hang')
        check(c == 1 and 'hard timeout after 4 s' in o, 'the hard timeout kills run_list and the game, exit 1', o)
        check(len(runs_lines()) == 8, 'every run, refused at the door or timed out included, logged its question first (%d lines)' % len(runs_lines()))
        # S110 SCENEMOM2: the finish ended the moment's wait (`until finish`): NOT ARRIVED and how the match ended, exit 1, not a generic failure
        c, o = go('Daniel Bryan vs Brock Lesnar at finish=pinfall : screenshot fin', '--exe', exe_new, mode='ended')
        check(c == 1 and 'ARRIVED  NO (NOT ARRIVED: the match ended first, by submission (winner_slot 0, frame 9031), while line 14 waited for '
              '`wait_event match what=finish finish_name=pinfall until match what=finish`; likely cause (not settled): ' in o
              and 'EXIT     1 not arrived (the match ended first: submission)' in o and 'never printed' not in o,
              'a moment the finish made impossible: NOT ARRIVED, how the match ended (finish_name) and the likely cause, exit 1', o)
        # S110 SCENEMOM2 review: the arrival line is shown whole: a table break's body_slot / body_id come after 200 characters
        c, o = go('Daniel Bryan vs Brock Lesnar; type=68 at table_break : screenshot b', '--exe', exe_new, mode='tablebreak')
        arr = [ln for ln in o.splitlines() if ln.startswith('ARRIVED')]
        check(c == 0 and len(arr) == 1 and arr[0].startswith('ARRIVED  yes (frame 14676, line ') and '"body_slot": 1, "body_id": 343}' in arr[0],
              'a table break arrived: the ARRIVED line keeps the bus line\'s fields to its end (body_slot, body_id) and its frame (the line\'s "f")', o)
        # S110 TOOLFIT: an Iron Man's falls carry the finish's fields (S110 ELIMFIN): the arrival is the finish line, not the first fall (run 299)
        c, o = go('Daniel Bryan vs Brock Lesnar at finish=pinfall : screenshot fin', '--exe', exe_new, mode='falls')
        arr = [ln for ln in o.splitlines() if ln.startswith('ARRIVED')]
        check(c == 0 and len(arr) == 1 and arr[0].startswith('ARRIVED  yes (frame 72217, line ') and '"what": "finish"' in arr[0] and '"winner_falls": 3' in arr[0],
              'a multi-fall match: ARRIVED cites the finish line (frame 72217, winner_falls), not the first fall that holds the same finish_name', o)
        # S110 TOOLFIT: the hold wish met only by a posture-family hold (finisher_fit_0 2): NOT ARRIVED right after the bell, in plain words
        c, o = go('Daniel Bryan vs Brock Lesnar at finish=submission : screenshot fin', '--exe', exe_fin, mode='fitfamily')
        check(c == 1 and 'ARRIVED  NO (NOT ARRIVED: seat 0\'s finisher wish `hold` is met only by a hold in a posture family (line 16, frame 3100: '
              'finisher_fit_0 is 2; slots 17 / 18 = 50326 / 58350: ' in o and 'the missing verb' in o and 'ids 58000..58099' in o
              and 'not settled' in o and 'never swaps the seats' in o and 'never printed' not in o and 'EXIT     1 not arrived' in o,
              'a hold wish only a posture-family hold meets: NOT ARRIVED with the reason (hedged) and the missing verb; the seats are never swapped', o)
        # S110 TOOLFIT review (rule 6): the other seat's pair is not read (a `finisher 1 any` line served only a hint)
        c, o = go('Daniel Bryan vs Brock Lesnar at finish=submission : screenshot fin', '--print', '--exe', exe_fin)
        check(c == 0 and 'finisher 0 hold' in o and 'finisher 1 any' not in o,
              'the submission recipe: seat 0\'s hold wish, no read of seat 1\'s pair', o)
        # 4. --live: a scene whose start needs boot flags is refused
        c, o = go('cas new at TOPS', '--live', 'main')
        check(c == 2 and '--wwe2k14_cas_goto=76' in o and 'boot flags' in o, '--live refuses a route that needs boot flags', o)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
        shutil.rmtree(scenes, ignore_errors=True)
    print('%d failed' % len(FAILS) if FAILS else 'all passed')
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
