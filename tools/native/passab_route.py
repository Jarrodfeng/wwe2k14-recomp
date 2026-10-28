r"""Session 72 part 4: write an in-run A/B route measured by the per-pass GPU log (gpu_native_truth_pass_log) instead of
the frame timestamps - the per-pass log reads the whole GPU frame from its first pass to its last, which is steadier
than ab_gpu.py's windows for changes of a few percent. At Brock's entrance +300, Bryan's +440 and the match +900 of the
s21_lb replay: CYCLES x (FRAMES pass-logged frames with the A values, FRAMES with the B values), labels
<scene>_a / <scene>_b; read with py -I tools\native\pass_roles.py RUN (sums per label) or passab_read.py.

    py -I tools\native\passab_route.py NAME "WHAT IT TESTS" CVAR=A,B [CVAR=A,B ...] [--cycles 4] [--frames 4]

Writes tools\input_scripts\short\s72_NAME_passab.txt. Run it with --wwe2k14_replay_play=s21_lb --gpu_native_gpu_timers=true
and the scene's renderer flags (e.g. --wwe2k14_preset_low=true --resolution_scale=2 and a test settings file).
"""
import argparse
import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HEAD = """set 0x0010 0 0 0 0 -1636 -1137
wait_frames 10
set 0x0000 0 0 0 0 -1069 -1234
wait_frames 66
set 0x1000 0 0 0 0 -770 -1136
wait_frames 10
set 0x0000 0 0 0 0 -770 -1136"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('name')
    ap.add_argument('what')
    ap.add_argument('cvars', nargs='+')
    ap.add_argument('--cycles', type=int, default=4)
    ap.add_argument('--frames', type=int, default=4)
    a = ap.parse_args()
    pairs = []
    for c in a.cvars:
        name, vals = c.split('=', 1)
        va, vb = vals.split(',', 1)
        pairs.append((name, va, vb))
    lines = ['# harness: fastforward',
             f'# Session 72 part 4, written by tools\\native\\passab_route.py: {a.what}.',
             f'# A = {", ".join(f"{n}={va}" for n, va, _ in pairs)}; B = {", ".join(f"{n}={vb}" for n, _, vb in pairs)}.',
             f'# {a.cycles} cycles x {a.frames} pass-logged frames each, labels <scene>_a / <scene>_b.',
             'on_fail quit', f'log bench: start s72_{a.name}_passab (launch)', 'wait_file SaveData.Dat 3600',
             'wait_frames 160', HEAD, 'wait_file MNUIHD.PAC 3600', 'wait_file INGAMELUA.PAC 1800',
             'log bench: match launched', 'wait_file BG04.PAC 900']
    gap = a.frames + 4
    for scene, marker, offset in (('brock', '343.bik2', 300), ('bryan', '324.bik2', 440), ('match', 'M3.PAC', 900)):
        lines += [f'wait_file {marker} 30000', f'log scene={scene}', f'wait_frames {offset}']
        for _ in range(a.cycles):
            for tag, idx in (('a', 1), ('b', 2)):
                for p in pairs:
                    lines.append(f'cvar {p[0]} {p[idx]}')
                lines += ['wait_frames 2', f'log passlog: {scene}_{tag}', f'cvar gpu_native_truth_pass_log {a.frames}',
                          f'wait_frames {gap}']
        for p in pairs:  # leave the B values between scenes
            lines.append(f'cvar {p[0]} {p[2]}')
    lines += ['wait_frames 60', 'log bench: done', 'quit']
    out = os.path.join(ROOT, 'tools', 'input_scripts', 'short', f's72_{a.name}_passab.txt')
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    print(out)


if __name__ == '__main__':
    main()
