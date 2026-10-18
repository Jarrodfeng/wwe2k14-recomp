"""Regenerate the benchmark route scripts from Jarrod's recording 056 (recorded_match_session4_b.txt).

Usage: py tools/input_scripts/build_bench_routes.py      (writes bench_*.txt next to this file)

Routes (all CPU vs CPU exhibitions, entrances on, unattended; need the frozen user data, see tools/bench):
  bench_lesnar_bryan.txt       Brock Lesnar (CH343) vs Daniel Bryan (CH324), Over the Limit (BG04)
  bench_lesnar_undertaker.txt  Brock Lesnar vs The Undertaker (CH103), Over the Limit (BG04)
  bench_taker_wm.txt           Brock Lesnar vs The Undertaker, WrestleMania Moment (BG56): CH103's heavy entrance
  bench_lesnar_bryan_state.txt the Lesnar vs Bryan route plus wait_state / assert_state checks on the menu and match
                               state (states.toml; Session 8 task 9a). Not a benchmark: the original stays that.
Menu facts (screenshots, runs 077-083):
  - Roster: the first (P1-side, COM) cursor starts on John Cena; Jarrod's RIGHT holds reach Lesnar. With the frozen
    save the second cursor starts on Undertaker (last pick in run 075), so A x3 picks him; Jarrod's moves reach Bryan.
  - Arena select opens on the randomly pre-selected arena (Jarrod, 2026-10-08), so relative moves are unreliable.
    Switching pages resets the cursor to the first tile: RB x3 (ARENAS -> WRESTLEMANIA -> CUSTOM -> ARENAS) = RAW.
    From RAW: UP wraps to Hell in a Cell (row 4), UP again = Over the Limit (row 3); UP, LEFT = WrestleMania Moment.
    Every route waits for its arena archive (wait_file BGnn.PAC), so a wrong arena fails the run.
  - When the pre-selected arena already is the target, the arena A press is ignored (no MATCHHD.PAC; runs 117, 142,
    143), so the pick uses press_until_file (Session 7b). After a retry the match setup inputs come later than in the
    recording (they follow MATCHHD.PAC rather than the first A), which only adds delay.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REC = os.path.join(HERE, 'recorded_match_session4_b.txt')


def route(*args):
    out = subprocess.run([sys.executable, os.path.join(HERE, 'make_route.py'), REC, *args],
                         capture_output=True, text=True, check=True).stdout
    return out.rstrip('\n').split('\n')


def drop_first_neutral(lines):
    # make_route --from emits a neutral 'set none' first; the previous part already ended with the pad released.
    return lines[1:] if lines and lines[0].startswith('set none') else lines


BOOT_TO_FIRST_PICK = ['--sync', 'SaveData.Dat@1057', '--sync', 'MNUIHD.PAC@1879', '--sync', 'GAME2DHD.PAC@2130',
                      '--sync', 'GAME2DHD.PAC@2164', '--sync', 'CH343.PAC@2298', '--timeout', '3600']

PICK_UNDERTAKER = [
    '# Second pick: the cursor already sits on Undertaker (frozen save); confirm like Jarrod confirmed Bryan.',
    'wait_frames 40',
    'set 0x1000 0 0 0 0 0 0', 'wait_frames 4', 'set 0x0000 0 0 0 0 0 0', 'wait_frames 6',
    'set 0x1000 0 0 0 0 0 0', 'wait_frames 4', 'set 0x0000 0 0 0 0 0 0', 'wait_frames 5',
    'set 0x1000 0 0 0 0 0 0', 'wait_file CH103.PAC 600',
]


def arena_nav(moves, bg):
    return [
        f'# Arena {bg}: RB x3 resets the cursor to RAW (the grid opens on the random pre-selected arena), then {" ".join(moves)}.',
        'wait_frames 40',
        'press RB 6 30', 'press RB 6 30', 'press RB 6 40',
        *[f'press {m} 6 45' for m in moves],
        'screenshot b_arena',
        # The match setup menu loads MATCHHD.PAC ~40 frames after a registered pick. When the random pre-selected
        # arena already is the target, the first A is ignored (runs 117, 142, 143: no file opens at all), so press
        # again if MATCHHD.PAC doesn't open (retries save captures/NNN_retry_l<line>_<n>.bmp).
        'press_until_file A matchhd.pac 3 90 5',
    ]


def tail(second_tron, entrance_frames):
    return [
        'log bench: match launched',
        'wait_file LOADHD.PAC 1200',
        f'wait_file {{bg}} 900',
        'wait_file 343.bik2 1800',
        'log scene=tron_343',
        'wait_frames 100',
        'screenshot b_brock100',
        f'wait_file {second_tron}.bik2 7200',  # 2x: entrances take twice the frames at 60 fps
        f'log scene=tron_{second_tron}',
        'wait_frames 440',
        'screenshot b_second440',
        f'wait_file LOADHD.PAC {entrance_frames}',
        'wait_file M3.PAC 900',
        'log scene=match',
        'wait_frames 600',
        'screenshot b_match600',
        'wait_frames 3000',
        'screenshot b_match3600',
        'wait_frames 60',  # PNG saves are async and slower than BMP: 30 frames lost the race at 4K (S18)
        'log bench: done',
        'quit',
    ]


def insert_at(lines, anchor, new, after=True, startswith=False, first_after=None):
    """Insert `new` after (or before) the one line equal to `anchor` (or starting with it). With first_after, the
    anchor is the first matching line after the one line equal to first_after. Fails if the anchor isn't unique, so
    a change in the generated route can't silently drop a check."""
    match = (lambda l: l.startswith(anchor)) if startswith else (lambda l: l == anchor)
    if first_after is not None:
        starts = [i for i, l in enumerate(lines) if l == first_after]
        assert len(starts) == 1, f'state variant: {first_after!r} found {len(starts)} times'
        hits = [i for i in range(starts[0] + 1, len(lines)) if match(lines[i])][:1]
    else:
        hits = [i for i, l in enumerate(lines) if match(l)]
    assert len(hits) == 1, f'state variant: anchor {anchor!r} found {len(hits)} times'
    i = hits[0] + (1 if after else 0)
    return lines[:i] + new + lines[i:]


def with_state_checks(lines, name):
    """Session 8 task 9a: the same route plus wait_state / assert_state / log_state checks on the game's menu and
    match state (tools/input_scripts/states.toml). Values and frames below are from the live peek logs of the
    Session 8 task 7 runs (run 239 = this route): seq 3 -> 75 at 1863, menu 1 at 1925 (PLAY highlighted, target 2),
    menu 2000 from 2032 (target -1) through character select and the arena grid, exit action 1 at 3262, seq 9 and
    switch kind 3 at 3294; match clock running from ~340 frames after the match scene (run 236). Every check sits at
    a point where the value is already reached in those runs. Checks placed before an input or a screenshot use
    HOLD 1, which takes no frames when the value is already there, so those inputs and screenshots land on the same
    frames as in the benchmark route (a wait only lasts longer when the game is slower than in the recording). A
    HOLD 2 wait takes a frame even then: in run 248 (all HOLD 2) each such wait took 1 frame, so the first menu A
    came 2 frames later after the MNUIHD.PAC open than in the benchmark route. Only the two waits after the last
    menu input (exit action, sequence 9) keep the default HOLD 2: nothing timed follows them before the next
    wait_file."""
    lines = [l.replace(f'log bench: start {name}', f'log bench: start {name}_state') for l in lines]
    header_end = lines.index('on_fail quit')
    lines = lines[:header_end] + [
        '#',
        '# STATE VARIANT (Session 8 task 9a, generated with the route above): the same inputs plus wait_state /',
        '# assert_state / log_state checks on the menu and match state (named in states.toml next to this file).',
        '# Checks before inputs and screenshots use HOLD 1 (no frames when the value is already there), so the',
        '# inputs land on the same frames as in the benchmark route while the game is on time.',
        f'# bench_{name}.txt stays the benchmark / regress route.',
    ] + lines[header_end:]
    lines = insert_at(lines, 'set 0x1000', [
        '# state: main menu up (sequence 75, SmartMenu menu 1 with PLAY highlighted) before the first A',
        'wait_state seq 75 600 1',
        'assert_state switch_kind 1',
        'wait_state menu 1 600 1',
        'assert_state target 2',
    ], after=False, startswith=True, first_after='wait_file MNUIHD.PAC 3600')
    lines = insert_at(lines, 'wait_file GAME2DHD.PAC', [
        '# state: past PLAY -> exhibition -> 1v1: the setup screens are SmartMenu menu 2000',
        'wait_state menu 2000 300 1',
    ], startswith=True, first_after='wait_file MNUIHD.PAC 3600')
    lines = insert_at(lines, '# Arena ', [
        '# state: still a setup screen (character select and the arena grid are both menu 2000, target -1)',
        'wait_state menu 2000 300 1',
        'assert_state target -1',
    ], startswith=True)
    lines = insert_at(lines, 'log bench: match launched', [
        '# state: exit action 1 (launch exhibition), then the in-game sequence 9 with switch kind 3',
        'wait_state exit_action 1 600',
        'wait_state seq 9 900',
        'assert_state switch_kind 3',
        'log_state smart_menu',
        'log_state scene_word',
        'log_state frame_rate',
    ], after=False)
    lines = insert_at(lines, 'log scene=tron_343', [
        'log_state scene_word',
        'log_state judge',
        'log_state frame_rate',
        'log_state match_clock',
    ])
    lines = insert_at(lines, 'screenshot b_match600', [
        '# state: in the ring the match clock runs, the referee object exists, still sequence 9',
        'wait_state match_clock >0 600 1',
        'assert_state seq 9',
        'assert_state judge !=0',
        'log_state scene_word',
        'log_state frame_rate',
    ])
    lines = insert_at(lines, 'log bench: done', ['log_state match_clock', 'assert_state seq 9'], after=False)
    return lines


def build(name, header, second, moves, bg, second_tron, entrance_frames, state=False):
    # Session 16: timed routes run at normal speed (run_list fast-forwards routes without this line).
    lines = ['# harness: realtime (timed route; Session 16: run_list fast-forwards other routes)'] + [
        f'# {l}' if l else '#' for l in header] + ['on_fail quit', f'log bench: start {name}']
    lines += route(*BOOT_TO_FIRST_PICK, '--until', '2346' if second == 'undertaker' else '2504')
    if second == 'undertaker':
        lines += PICK_UNDERTAKER
        lines += drop_first_neutral(route('--from', '2506', '--sync', 'CH153.PAC@2665', '--until', '2766'))
    else:
        # Bryan's pick ends with an A press at 2504 (CH324 loads at 2506); continue from there.
        lines += ['wait_file CH324.PAC 600']
        lines += drop_first_neutral(route('--from', '2506', '--sync', 'CH153.PAC@2665', '--until', '2766'))
    lines += arena_nav(moves, bg)
    # After the arena pick: Jarrod's RT (match setup) and A (PLAY), with his timing. No sync on the EVTMOV.PAC load
    # in between: it doesn't happen when the randomly pre-selected arena already is the one picked (run 117 failed
    # there), so the recorded delay from the arena A press is used instead.
    lines += drop_first_neutral(route('--from', '2840', '--sync', 'AUDIENCE.PAC@2966',
                                      '--until', '2975'))
    lines += [l.replace('{bg}', bg) for l in tail(second_tron, entrance_frames)]
    with open(os.path.join(HERE, f'bench_{name}.txt'), 'w', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    print(f'bench_{name}.txt: {len(lines)} lines')
    if state:
        state_lines = with_state_checks(lines, name)
        with open(os.path.join(HERE, f'bench_{name}_state.txt'), 'w', newline='\n') as f:
            f.write('\n'.join(state_lines) + '\n')
        print(f'bench_{name}_state.txt: {len(state_lines)} lines')


COMMON = ['Generated by build_bench_routes.py from Jarrod\'s recording 056 (do not edit by hand; edit the generator).',
          'Needs the frozen user data: --user_data_root/--cache_root pointing at a copy of out\\bench\\userdata_frozen',
          '(tools/bench/run_bench.ps1 does this). Screenshots b_* are probes for checking the route (local only).']
build('lesnar_bryan', ['Benchmark route: Brock Lesnar vs Daniel Bryan, Over the Limit (BG04). Scenes: 343.bik2 = Brock\'s',
                       'entrance, 324.bik2 = Bryan\'s (ramp pan), M3.PAC = match.'] + COMMON,
      'bryan', ['UP', 'UP'], 'BG04.PAC', '324', 9000, state=True)
build('lesnar_undertaker', ['Benchmark route: Brock Lesnar vs The Undertaker (CH103), Over the Limit (BG04).'] + COMMON,
      'undertaker', ['UP', 'UP'], 'BG04.PAC', '103', 16000)
build('taker_wm', ['Benchmark route: Brock Lesnar vs The Undertaker (CH103) in WrestleMania Moment (BG56): CH103\'s',
                   'heavy entrance (Jarrod\'s run 075; reproduced in run 083: ~20 s at 22-27 fps).'] + COMMON,
      'undertaker', ['UP', 'LEFT'], 'BG56.PAC', '103', 16000)
