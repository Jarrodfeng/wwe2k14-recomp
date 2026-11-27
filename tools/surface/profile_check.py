"""S102 lane S0-1: the Surface bench's settings profiles are what a player on that machine gets.

  py -I tools\\surface\\profile_check.py             checks the three profiles in tools\\surface\\profiles
  py -I tools\\surface\\profile_check.py --selftest  plants faults and passes only when every one is caught (the twin)

The check feeds each profile through the game's own settings reader, tier resolution and flag list (gfx_applied.exe, built from
src\\gfx_applied.h + src\\gfx_settings.h, the same code the game logs its `[gfx][applied]` line with) and compares:
  auto             == a fresh install on integrated graphics (gfx_applied --fresh, frame rate forced to the bench's 60), byte for byte AND by
                      fields; tier LOW, 720p, crowd 70 %, true_native 1, the LOW levers on (shadow half rate, SGSR);
  standard_player  == the same install after the player picked STANDARD (--fresh --tier 2): tier STANDARD, crowd 100 %, true_native 1;
  release_day      == release day's file, byte for byte, kept for continuity: tier STANDARD, true_native 0 (no settings_version).
Also: `--fresh` without --frame-rate must give frame_rate 3 (what a fresh install writes); the shared tier function equals the old inline block
over a truth table (and a planted-wrong copy must not); the game's code still logs the line; bench.ps1 is an advanced script (a misspelt
parameter throws).
"""
import argparse, os, re, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXE = ROOT / 'out' / 'tools' / 'gfx_applied.exe'
CLI_SRC = [ROOT / 'tools' / 'surface' / 'gfx_applied_cli.cpp', ROOT / 'src' / 'gfx_applied.h', ROOT / 'src' / 'gfx_settings.h']
GPU = ['--gpu', 'integrated', '--gpu-name', 'Intel(R) UHD Graphics 620']
PANEL = '2736x1824'  # the Surface Pro 6's panel
RELEASE_DAY_TEXT = '{"renderer": 2, "frame_rate": 1, "resolution": 0, "quality_tier": 2}'  # bench.ps1 until S102 S0-1 (release day)
KINDS = {  # profile -> (extra --fresh args, expected fields)
    'auto': (['--frame-rate', '1'], {'tier': 'LOW', 'scale': '1', 'internal': '1280x720', 'crowd': '70', 'tn': '1', 'fps': '1', 'swapchain': PANEL}),
    'standard_player': (['--tier', '2', '--frame-rate', '1'],
                        {'tier': 'STANDARD', 'scale': '1', 'internal': '1280x720', 'crowd': '100', 'tn': '1', 'fps': '1', 'swapchain': PANEL}),
}


def lf(data):
    return data.replace(b'\r\n', b'\n')


def build():
    if EXE.exists() and EXE.stat().st_mtime >= max(p.stat().st_mtime for p in CLI_SRC):
        return
    r = subprocess.run(['pwsh', '-NoProfile', '-File', str(ROOT / 'tools' / 'surface' / 'build_gfx_applied.ps1')], capture_output=True, text=True, timeout=300)
    if r.returncode != 0 or not EXE.exists():
        raise SystemExit('FAIL: gfx_applied build failed:\n' + r.stdout + r.stderr)


def run(args, ok=(0,)):
    r = subprocess.run([str(EXE)] + args, capture_output=True, text=True, timeout=60)
    if r.returncode not in ok:
        raise SystemExit('FAIL: gfx_applied %s -> exit %d: %s' % (' '.join(args), r.returncode, r.stderr.strip()))
    return r.stdout.strip()


def applied_line(settings_path):
    return run(['--settings', str(settings_path), '--panel', PANEL] + GPU)


def fields(line):
    m = re.match(r'\[gfx\]\[applied\] tier=(LOW|STANDARD) \((.*?)\) render_scale=(\d+) internal=(\d+x\d+) swapchain=(\S+) crowd=(\d+)% '
                 r'frame_rate=(\d+) true_native=(\d) levers on: (.*?); ignored \(asked, new renderer does not read\): (.*)$', line)
    if not m:
        raise SystemExit('FAIL: the applied line does not parse: ' + line)
    return dict(zip(['tier', 'why', 'scale', 'internal', 'swapchain', 'crowd', 'fps', 'tn', 'on', 'ignored'], m.groups()))


def field_errors(path, kind):
    """What the game would run for this file, against what the profile is for. No byte comparison here."""
    errors = []
    line = applied_line(path)
    f = fields(line)
    for k, v in KINDS[kind][1].items():
        if f[k] != v:
            errors.append('%s: %s is %s, want %s (%s)' % (kind, k, f[k], v, line))
    if kind == 'auto':
        for lever in ('gpu_shadow_half_rate', 'present_effect=sgsr'):
            if lever not in f['on']:
                errors.append('auto: LOW lever %s is not on (levers on: %s)' % (lever, f['on']))
    else:
        # S102 (Jarrod 2026-11-14 19:5x): the half-rate shadow maps are every tier's default now; STANDARD has that one lever and no other.
        if f['on'] != 'gpu_shadow_half_rate':
            errors.append('%s: STANDARD must have exactly gpu_shadow_half_rate on (every tier since 2026-11-14) and no LOW lever (levers on: %s)' % (kind, f['on']))
    return errors, line


def byte_errors(path, kind):
    """The file is what the launcher's rule writes (gfx_applied --fresh), not a hand copy."""
    with tempfile.TemporaryDirectory() as d:
        fresh = Path(d) / 'fresh.json'
        run(['--fresh', '--out', str(fresh)] + KINDS[kind][0] + GPU)
        if lf(fresh.read_bytes()) != lf(Path(path).read_bytes()):  # (git may check the file out with CRLF)
            return ['%s: %s differs from what gfx_applied --fresh %s writes' % (kind, path, ' '.join(KINDS[kind][0]))]
    return []


def check_profile(path, kind, errors, bytes_too=True):
    path = Path(path)
    if not path.is_file():
        errors.append('%s profile missing: %s' % (kind, path))
        return None
    e, line = field_errors(path, kind)
    errors.extend(e)
    if bytes_too:
        errors.extend(byte_errors(path, kind))
    return line


def check_release_day(path, errors):
    path = Path(path)
    if not path.is_file():
        errors.append('release_day profile missing: %s' % path)
        return None
    line = applied_line(path)
    f = fields(line)
    if f['tier'] != 'STANDARD' or f['crowd'] != '100' or f['tn'] != '0':
        errors.append('release_day: wrong tier / crowd / true_native: ' + line)
    if path.read_text(encoding='utf-8').strip() != RELEASE_DAY_TEXT:
        errors.append('release_day: %s is not release day\'s file' % path)
    return line


def check_fresh_default(errors):
    with tempfile.TemporaryDirectory() as d:
        p = Path(d) / 'fresh.json'
        run(['--fresh', '--out', str(p)] + GPU)
        m = re.search(r'"frame_rate":\s*(\d+)', p.read_text(encoding='utf-8'))
        if not m or m.group(1) != '3':
            errors.append('--fresh without --frame-rate gives frame_rate %s, a fresh integrated install writes 3' % (m.group(1) if m else '?'))


def check_truth_table(errors):
    out = run(['--truth-table'], ok=(0, 1))
    m = re.search(r'(\d+) cases, (\d+) mismatches', out)
    if not m or int(m.group(1)) < 200 or m.group(2) != '0':
        errors.append('ResolveLow differs from the old inline block: ' + out)
    out = run(['--truth-table', '--planted'], ok=(0, 1))
    m = re.search(r'(\d+) cases, (\d+) mismatches', out)
    if not m or m.group(2) == '0':
        errors.append('the truth table did not catch a planted wrong copy: ' + out)


def check_sources(errors):
    menu = (ROOT / 'src' / 'gfx_menu.h').read_text(encoding='utf-8', errors='replace')
    if 'gfxs::AppliedLine(' not in menu or 'ResolveLow(' not in menu:
        errors.append('src\\gfx_menu.h no longer logs the [gfx][applied] line through gfx_applied.h')
    if 'applied_calls == 2' not in menu:
        errors.append('src\\gfx_menu.h logs [gfx][applied] on every call (ApplyLauncherSettings runs twice per boot)')
    sys.path.insert(0, str(ROOT / 'tools' / 'split'))
    from read_unit import read_text  # launcher_main.cpp is split: read it with its parts expanded (text mode: '\n' line ends)
    launcher = read_text(ROOT / 'src' / 'launcher' / 'launcher_main.cpp')
    if 'SuggestIntegrated(' not in launcher:
        errors.append('the launcher no longer takes the integrated first-run values from gfx_applied.h')
    bench = (ROOT / 'tools' / 'surface' / 'bench.ps1').read_text(encoding='utf-8', errors='replace')
    if 'profiles' not in bench or '$Profile' not in bench or '$DryRun' not in bench:
        errors.append('tools\\surface\\bench.ps1 no longer has -Profile / -DryRun reading tools\\surface\\profiles')
    # A misspelt bench_args key must throw (CmdletBinding), not run silently.
    r = subprocess.run(['pwsh', '-NoProfile', '-Command',
                        '$p=@{Profle="auto"; DryRun=$true; Out="out\\s102\\s0_1_typo"}; try { & "%s" @p; "RAN" } catch { "THREW" }' % (ROOT / 'tools' / 'surface' / 'bench.ps1')],
                       capture_output=True, text=True, timeout=120)
    if r.stdout.strip().splitlines()[-1:] != ['THREW']:
        errors.append('bench.ps1 with a misspelt parameter (Profle) did not throw: ' + r.stdout.strip()[-200:])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--selftest', action='store_true')
    ap.add_argument('--profiles', default=str(ROOT / 'tools' / 'surface' / 'profiles'))
    args = ap.parse_args()
    build()
    prof = Path(args.profiles)
    if not args.selftest:
        errors = []
        lines = [check_profile(prof / 'auto.json', 'auto', errors),
                 check_profile(prof / 'standard_player.json', 'standard_player', errors),
                 check_release_day(prof / 'release_day.json', errors)]
        check_fresh_default(errors)
        check_truth_table(errors)
        check_sources(errors)
        if errors:
            print('FAIL:\n  ' + '\n  '.join(errors))
            return 1
        print('\n'.join(l for l in lines if l))
        print('OK: auto = a fresh integrated install (LOW, 720p, crowd 70 %, levers on); standard_player = the same with STANDARD (true_native 1); '
              'release_day = release day\'s file; fresh frame rate 3; tier function == the old block (truth table)')
        return 0
    # Twin: each planted fault must be caught.
    good = (prof / 'auto.json').read_text(encoding='utf-8')
    sp = (prof / 'standard_player.json').read_text(encoding='utf-8')
    # A hand-written file: other key order and spacing, otherwise the auto file, with crowd 100 (the fault). No byte match is possible, so
    # only the field check can see it.
    hand = '{"settings_version":4,"true_native":true,"crowd_density":100,\n "quality_tier":0, "upscale":0,"draw_height":0,\n\n  "resolution":0,"frame_rate":1,"renderer":2}'
    faults = [
        ('auto', 'both LOW keys missing (settings_version, true_native: the release-day fault, LOW levers off)',
         re.sub(r',\n\s*"true_native": true,\n\s*"settings_version": \d+', '', good), True),
        ('auto', 'true_native missing', re.sub(r',\n\s*"true_native": true', '', good), True),
        ('auto', 'crowd 100', good.replace('"crowd_density": 70', '"crowd_density": 100'), True),
        ('auto', 'tier STANDARD', good.replace('"quality_tier": 0', '"quality_tier": 2'), True),
        ('auto', 'resolution 1440p', good.replace('"resolution": 0', '"resolution": 1'), True),
        ('auto', 'hand-written file (other key order / spacing) with crowd 100: the FIELD check alone must catch it', hand, False),
        ('standard_player', 'standard_player without true_native', re.sub(r',\n\s*"true_native": true', '', sp), True),
        ('standard_player', 'standard_player at tier AUTO (LOW levers / crowd 70 would apply)', sp.replace('"quality_tier": 2', '"quality_tier": 0'), True),
    ]
    missed = []
    with tempfile.TemporaryDirectory() as d:
        for kind, name, text, with_bytes in faults:
            if text == good or text == sp:
                raise SystemExit('selftest broken: the planted fault "%s" changed nothing' % name)
            p = Path(d) / 'x.json'
            p.write_bytes(lf(text.encode('utf-8')))
            errors = []
            check_profile(p, kind, errors, bytes_too=with_bytes)
            print('%-100s %s' % (name, 'caught' if errors else 'MISSED'))
            if not errors:
                missed.append(name)
        # The unmodified files must pass (a checker that fails everything proves nothing).
        for kind, text in (('auto', good), ('standard_player', sp)):
            p = Path(d) / 'x.json'
            p.write_bytes(lf(text.encode('utf-8')))
            errors = []
            check_profile(p, kind, errors)
            if errors:
                print('the good %s file fails: %s' % (kind, '; '.join(errors)))
                return 1
        # fresh default: a --fresh that quietly took frame rate 1 must be seen (the check reads the CLI; here we only show the check can fail)
        errors = []
        check_fresh_default(errors)
        if errors:
            print('fresh default fails: ' + '; '.join(errors))
            return 1
        r = re.search(r'"frame_rate":\s*(\d+)', good)
        if not r or r.group(1) != '1':
            print('selftest broken: the committed auto file is expected to carry frame_rate 1')
            return 1
    if missed:
        print('FAIL: not caught: ' + ', '.join(missed))
        return 1
    print('OK: all %d planted faults caught, the good files pass' % len(faults))
    return 0


if __name__ == '__main__':
    sys.exit(main())
