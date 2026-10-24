"""Session 56: write an in-run A/B route on the s21_lb replay (Brock's and Bryan's entrances, the match), in the form of
the Session 52 A/B routes (short\\s52_rename_ab.txt): settings switched on / off every 120 frames for 40 cycles from
Brock's titantron on, logged as "ab: mode 1 / 0".

    py tools\\perf\\s56_ab_route.py NAME "WHAT IT TESTS" CVAR[=ON,OFF] [CVAR[=ON,OFF] ...]

CVAR alone switches true / false; CVAR=ON,OFF gives the two values (mode 1 = ON). Writes
tools\\input_scripts\\short\\s56_NAME_ab.txt. Run it with
--wwe2k14_replay_play=s21_lb --wwe2k14_preset_low=true --vulkan_gpu_timestamps=true --wwe2k14_file_trace=true
[--resolution_scale=2 for 1440p] and read it with py tools\\native\\ab_gpu.py RUN [--metric busy].
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'input_scripts', 'short')

HEAD = """on_fail quit
log bench: start s56_{name}_ab (launch)
wait_file SaveData.Dat 3600
wait_frames 160
set 0x0010 0 0 0 0 -1636 -1137
wait_frames 10
set 0x0000 0 0 0 0 -1069 -1234
wait_frames 66
set 0x1000 0 0 0 0 -770 -1136
wait_frames 10
set 0x0000 0 0 0 0 -770 -1136
wait_file MNUIHD.PAC 3600
log bench: main menu reached (the launcher fires after its idle frames)
wait_file INGAMELUA.PAC 1800
log bench: match launched
wait_file 343.bik2 1800
repeat 40
"""


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    name, what = sys.argv[1], sys.argv[2]
    switches = []
    for arg in sys.argv[3:]:
        cvar, _, values = arg.partition('=')
        on, off = values.split(',') if values else ('true', 'false')
        switches.append((cvar, on, off))
    lines = [f'# harness: fast-forward ok (Session 56: in-run A/B - {what}; GPU ms and command-thread busy per frame)',
             '# (Brock\'s and Bryan\'s entrances, the match), logged as "ab: mode 1 / 0". Run with --wwe2k14_replay_play=s21_lb',
             '# --wwe2k14_preset_low=true --vulkan_gpu_timestamps=true --wwe2k14_file_trace=true [--resolution_scale=2 for',
             '# 1440p]; read with py tools\\native\\ab_gpu.py RUN [--metric busy]. Made by tools\\perf\\s56_ab_route.py. on_fail quit.']
    text = '\n'.join(lines) + '\n' + HEAD.format(name=name)
    text += ''.join(f'cvar {c} {on}\n' for c, on, _ in switches) + 'log ab: mode 1\nwait_frames 120\n'
    text += ''.join(f'cvar {c} {off}\n' for c, _, off in switches) + 'log ab: mode 0\nwait_frames 120\n'
    text += 'end_repeat\nlog bench: done\nquit\n'
    path = os.path.join(OUT, f's56_{name}_ab.txt')
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    print(os.path.normpath(path))


if __name__ == '__main__':
    main()
