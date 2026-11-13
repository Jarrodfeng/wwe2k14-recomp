# Gate check of lane 254 (S101): the props hook stays idle unless the arena record is a real match (PropsCore::InMatch, src\renderer_scene\props_ours_core.h).
#   py -I tools\props\props_gate_selftest.py [--step N]
# Runs out\tools\props_ours_test.exe --gate-synth / --gate (built by tools\props\build_props_ours_test.ps1): no game files, no GPU, 0 game runs.
#  (1) synthetic records: editor ids (bg00 one actor, bg23 none) idle; match ids load; unknown arena keeps the old rule; menu-only backgrounds never load.
#  (2) the SCENE RECORDS of existing captures (only the capture\scene\*.rsc members of each case zip are unpacked, to <main checkout>\out\lane254\<name>\scene; never committed):
#      idle: the Create-a-Superstar editor walk (bg00), its clothing pages sweep (bg23); load: a match, WM30, a Bryan entrance, a victory sweep, a 2-wrestler match type.
#  (3) the PLANTS special_ids (people required only for arenas 0 / 23) and editor_as_match (the pre-lane-254 rule: any named arena is a match) must FAIL, on the editor cases and nowhere else.
# Exit 0 all hold; 1 a check failed; 2 an input is missing (loud).
import argparse, glob, re, shutil, subprocess, sys, zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parents[2]


def main_checkout():
    try:
        common = subprocess.run(['git', '-C', str(HERE), 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return Path(common).parent
    except Exception:
        pass
    return HERE


MAIN = main_checkout()
fails = []

# name -> (run folder glob under out\s101\runs, expectation)
CASES = {
    'caw_walk': ('*_s101_checkB_caw_walk', 'idle'),         # the editor: bg00, 1 actor, no crowd (log: props ours frames 14 after 163 not ready, objects joined [])
    'caw_pages': ('*_s101_sweep_caw_pages', 'idle'),        # the clothing pages: bg23, no actor, no crowd
    'match': ('*_s100_owner_match', 'load'),
    'wm30': ('*_s100_owner_s90_wm30', 'load'),
    'bryan_entrance': ('*_s100_owner_bryan_entrance', 'load'),
    'victory': ('*_s101_sweep_victory', 'load'),
    'match_type': ('*_s101_sweep_match_type', 'load'),
}


def check(name, ok, detail=''):
    print(('PASS  ' if ok else 'FAIL  ') + name + (f'  ({detail})' if detail else ''))
    if not ok:
        fails.append(name)


def run(exe, *args):
    p = subprocess.run([str(exe), *[str(a) for a in args]], capture_output=True, text=True, timeout=1200)
    return p.returncode, p.stdout, p.stderr


def unpack(name, pattern):
    out = MAIN / 'out' / 'lane254' / name
    if list((out / 'scene').glob('*.rsc')):
        return out
    dirs = sorted(glob.glob(str(MAIN / 'out' / 's101' / 'runs' / pattern)))
    for d in reversed(dirs):
        zips = sorted(glob.glob(str(Path(d) / 'case_*.zip')))
        if not zips:
            continue
        with zipfile.ZipFile(zips[-1]) as z:
            members = [m for m in z.namelist() if m.startswith('capture/scene/') and m.endswith('.rsc')]
            if not members:
                continue
            shutil.rmtree(out, ignore_errors=True)
            (out / 'scene').mkdir(parents=True)
            for m in members:
                (out / 'scene' / Path(m).name).write_bytes(z.read(m))
        return out
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--step', type=int, default=5, help='read every Nth record')
    a = ap.parse_args()
    exe = HERE / 'out' / 'tools' / 'props_ours_test.exe'
    if not exe.exists():
        exe = MAIN / 'out' / 'tools' / 'props_ours_test.exe'
    if not exe.exists():
        print('FAIL  props_ours_test.exe missing: run tools\\props\\build_props_ours_test.ps1')
        return 2
    rc, out, err = run(exe, '--gate-synth')
    check('synthetic.decision', rc == 0 and 'RESULT OK' in out and out.count('PASS  gate.synth') == 13, f'exit {rc}')
    inputs, expect = [], {}
    for name, (pat, want) in CASES.items():
        d = unpack(name, pat)
        if d is None:
            print(f'FAIL  input.{name}: no case zip with scene records under out\\s101\\runs\\{pat} (loud)')
            return 2
        inputs += ['--gate', f'{d}={want}']
        expect[name] = want
    rc, out, err = run(exe, '--step', a.step, *inputs)
    check('records.clean', rc == 0 and 'RESULT OK fault=none' in out, f'exit {rc}\n' + '\n'.join(l for l in out.splitlines() if l.startswith('FAIL')))
    for name, want in expect.items():
        print('   ' + next((l for l in out.splitlines() if f'gate.capture.{name}.{want}' in l), f'(no line for {name})')[:200])
    # the plant: the old rule must fail exactly the editor cases (the match cases still load under the old rule)
    rc, out, err = run(exe, '--step', a.step, *inputs, '--fault', 'editor_as_match')
    failed = set(re.findall(r'^FAIL  gate\.(\S+)', out, re.M))
    editor = {f'synth.editor_bg00_one_actor', 'synth.editor_pages_bg23_none', 'synth.editor_bg00_empty', 'synth.editor_like_bg04_no_people', 'synth.editor_like_bg44_one_actor', 'synth.menu_bg66_never_a_match', 'synth.menu_bg68_never_a_match', 'synth.menu_bg80_never_a_match',
              'capture.caw_walk.idle', 'capture.caw_pages.idle'}
    check('plant.editor_as_match', rc == 1 and failed == editor, f'exit {rc}, failed {sorted(failed)}')
    rc, out, err = run(exe, '--step', a.step, *inputs, '--fault', 'special_ids')
    failed = set(re.findall(r'^FAIL  gate\.(\S+)', out, re.M))
    check('plant.special_ids', rc == 1 and failed == {'synth.editor_like_bg04_no_people', 'synth.editor_like_bg44_one_actor'}, f'exit {rc}, failed {sorted(failed)} (an editor-like record of an arena other than 0 / 23 must be idle)')
    print('RESULT ' + ('FAIL' if fails else 'OK'), fails)
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
