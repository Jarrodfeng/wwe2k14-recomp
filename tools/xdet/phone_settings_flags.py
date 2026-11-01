"""Session 77: the game settings of a phone run as PC flags, so a PC play can run with the phone's requested renderer
and game settings.

    py tools/xdet/phone_settings_flags.py out/android/runs/120/logs/det/det_files_148.csv
    -> one line of --name=value flags (paths, Android-only and per-run settings left out)

The same REQUESTED settings, not necessarily the same effective renderer: the SDK falls back by itself where a device
lacks a feature and leaves the setting as it was (comparer skeptics, wf_9dc5ff67-b15: phone 120 asked for
render_target_path_vulkan=fsi and ran host render targets - the Adreno has no shader interlock,
render_target_cache.cpp - while PC 4901 ran the interlock path). The '# xdet' lines do not show effective paths yet.

Reads the '# xdet modified:' line of the run's det CSV (the CSV writer turns commas and spaces in a value into '_';
values with those characters are left out and named on stderr, as are absolute paths). Read-only."""
import re
import sys

# Not the phone's to give the PC: paths / folders, Android-only settings, what run_stageA.ps1 sets per run, and the
# per-run test and harness switches (a phone poke would re-poke the PC). Session 77 (skeptics): scenario_dir etc. added.
SKIP = re.compile(r'^(user_data_root|cache_root|game_data_root|gpu_anatomy_path|det_serial_trace|wwe2k14_det_hash_dir|'
                  r'wwe2k14_replay_dir|wwe2k14_replay_play|wwe2k14_replay_record|wwe2k14_input_script|'
                  r'wwe2k14_settings_file|wwe2k14_android_|wwe2k14_det_hash|det_frames$|det_serial$|'
                  r'wwe2k14_fixed_seed|vulkan_device$|log_|wwe2k14_scenario_dir$|wwe2k14_det_poke$|wwe2k14_live|'
                  r'wwe2k14_screenshot|wwe2k14_check_|wwe2k14_timeline|wwe2k14_bookmark|wwe2k14_clip|'
                  r'det_serial_trace_detail|window_|fullscreen|wwe2k14_background_full_speed)')


def main(path):
    line = next((l for l in open(path, encoding='utf-8', errors='replace') if l.startswith('# xdet modified:')), None)
    if not line:
        sys.exit('%s: no "# xdet modified" line' % path)
    flags, left = [], []
    for kv in line[len('# xdet modified:'):].split():
        k, _, v = kv.partition('=')
        if SKIP.match(k):
            continue
        if '_' in v or v.startswith('/'):  # maybe a comma or space the writer replaced, or a phone path: not guessed
            left.append(k)
            continue
        flags.append('--%s=%s' % (k, v))
    if left:
        print('left out (value may hold a comma or space, or is a path): ' + ', '.join(left), file=sys.stderr)
    print(' '.join(flags))


if __name__ == '__main__':
    main(sys.argv[1])
