#!/usr/bin/env python3
r"""S102 lane U3: other graphics cards on the PC. 0 game runs: fview replays two kept case zips with the Khronos profiles layer simulating a card.

  py -I tools\vkprofiles\cards_check.py [--root DIR] [--only PROFILE] [--sheets DIR]

Layer order (review U3-1): the loader gets VK_INSTANCE_LAYERS=validation;profiles so VALIDATION IS ABOVE the profiles layer and checks against the simulated card, not
the real RTX 4090. The order is asserted from the loader's own VK_LOADER_DEBUG=layer chain (the loader prints the chain bottom-first, so profiles must be listed before
validation; the planted push-constant profile proves it by behaviour: it raises validation errors only when validation sees the simulated limit).
Per profile (amd_vega_680m, intel_uhd_620, mali_g615) and per capture (the title / boot-menu run, Bryan's entrance run), validation on:
  applied   the renderer's `[rdr] device:` line carries the profile's own Vulkan version and BC / ASTC flags (make_profiles.META); a planted run with a
            misspelt profile_name must NOT satisfy that check
  clean     0 validation errors, exit 0, frames > 0, the last frame decodes to a picture with colours, NO `PROFILES WARNING` line (a profile can only lower what the real
            device offers; any warning means a value was raised or a feature does not exist on this PC), the layer chain order above
  same      the intel and mali pictures equal the AMD profile's within a tolerance (mean per-channel difference and fraction of changed pixels); a black title with a
            few UI bits fails (planted, run in-process)
  sheet     one picture sheet per card in out\s102\sheets\u3 (KEEP: reason + date 7 days out)
Planted twins: plant_no_rgba16f (the floor must refuse it), plant_small_push (validation errors > 0 under validation-above-profiles), a typo'd profile name (applied fails).
Mali: BC is off. Until U1's CPU BC decode lands (fview --bc-decode) the renderer has no BC fallback and this is a KNOWN FINDING the check expects to the letter
(image_format_unsupported on BC1-BC5, the dev build aborts). Once `--bc-decode` exists in src\fview\main.cpp the finding is removed: Mali must pass the normal checks and log
`the device CANNOT sample them; bc_decode 1 -> decoded on the CPU` with no image_format_unsupported (AMD / Intel: `the device samples them`), and a planted twin with
`--bc-decode 0` must exit non-zero with `image_format_unsupported: format 2[4-8],`.
A missing capture, exe, layer or profile FAILs loudly (never skips).
"""
import argparse
import datetime
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import make_profiles as mp  # noqa: E402

PROFILES = ('amd_vega_680m', 'intel_uhd_620', 'mali_g615')
CAPTURES = {
    'title': 'out/s101/runs/20261112_122344_s100_owner_boot_menu_quit/case_20261112_122351_841_0_pc_run_exit.zip',
    'bryan': 'out/s101/runs/20261112_122700_s100_owner_bryan_entrance/case_20261112_122719_969_0_pc_run_exit.zip',
}
# S102 U3 finding (until U1 lands): with textureCompressionBC off the renderer has no BC fallback; the first BC image is image_format_unsupported (format 24..28 = BC1..BC5)
# and the dev build aborts; a player build counts it and draws the transparent dummy.
KNOWN_FINDINGS = {'mali_g615': (r'image_format_unsupported: format 2[4-8],', 'BC texture refused, no fallback')}
MAIN = Path(r'C:\Users\Jarrod\wwe2k14-recomp')
LAYER = 'VK_LAYER_KHRONOS_profiles'
VALIDATION = 'VK_LAYER_KHRONOS_validation'
results = []


def report(name, ok, evidence):
    results.append(ok)
    print(f'{"PASS" if ok else "FAIL"}  {name}: {evidence}', flush=True)


def layer_env(profile, scratch, profile_name=None, order=(VALIDATION, LAYER)):
    """The environment that puts validation ABOVE the profiles layer (first in VK_INSTANCE_LAYERS = nearest the app) and names the profile."""
    d = scratch / f'settings_{profile_name or profile}'
    d.mkdir(parents=True, exist_ok=True)
    (d / 'vk_layer_settings.txt').write_text(
        'khronos_profiles.profile_emulation = true\n'
        f'khronos_profiles.profile_dirs = {HERE.as_posix()}\n'
        f'khronos_profiles.profile_name = {profile_name or "VP_WWE_" + profile}\n'
        'khronos_profiles.simulate_capabilities = SIMULATE_API_VERSION_BIT,SIMULATE_FEATURES_BIT,SIMULATE_PROPERTIES_BIT,SIMULATE_EXTENSIONS_BIT,SIMULATE_FORMATS_BIT\n'
        'khronos_profiles.profile_validation = true\n'
        'khronos_profiles.emulate_portability = false\n')   # (true makes the device offer VK_KHR_portability_subset, which validation then demands be enabled)
    env = dict(os.environ)
    env['VK_LAYER_SETTINGS_PATH'] = str(d)
    env['VK_INSTANCE_LAYERS'] = ';'.join(order)
    env.pop('VK_LOADER_LAYERS_ENABLE', None)
    env['VK_LOADER_DEBUG'] = 'layer'
    return env


def layer_installed():
    p = subprocess.run(['vulkaninfo', '--summary'], capture_output=True, text=True, timeout=120)
    return LAYER in p.stdout + p.stderr and VALIDATION in p.stdout + p.stderr


def chain_order(text):
    """The instance layers in the order the loader printed them."""
    return re.findall(r'Insert instance layer "([^"]+)"', text)


def validation_above_profiles(text):
    """The loader prints the chain bottom-first (the layer nearest the driver first): profiles listed BEFORE validation = validation is above profiles. Proven by behaviour
    by the plant_small_push twin (errors only appear when validation sees the simulated limit) and by the 'portability_subset' error that appeared only in this order."""
    c = chain_order(text)
    return LAYER in c and VALIDATION in c and c.index(LAYER) < c.index(VALIDATION)


def device_line(text):
    m = re.search(r'\[rdr\] device: (.+?), Vulkan (\d+\.\d+\.\d+), driver ([0-9A-Fa-f]+), unified memory \d+, lazy memory \d+, BC (\d), ASTC (\d)', text)
    return m.groups() if m else None


def applied(text, profile):
    """(ok, why): the renderer saw the profile's card: its Vulkan version (distinct per profile where it can be), BC and ASTC (the layer does not simulate deviceName / driverVersion)."""
    want = mp.META[profile]
    got = device_line(text)
    if not got:
        return False, 'no [rdr] device: line'
    name, ver, drv, bc, astc = got
    ok = ver == want['vulkan'] and int(bc) == want['bc'] and int(astc) == want['astc']
    return ok, f'"{name}" Vulkan {ver} driver {drv} BC {bc} ASTC {astc}; the profile says Vulkan {want["vulkan"]} BC {want["bc"]} ASTC {want["astc"]}'


def profile_warnings(text):
    return sorted({re.sub(r'\s+', ' ', l.strip())[:170] for l in text.splitlines() if 'PROFILES WARNING' in l})


def fview(exe, root, cap, scratch, env, dump_dir=None, frames=0, extra=()):
    cmd = [str(exe), '--root', str(root), '--case', str(cap), '--replay', '--validation', '1'] + list(extra)
    if frames:
        cmd += ['--frames', str(frames)]
    if dump_dir is not None:
        cmd += ['--dump-frames', ','.join(str(i) for i in range(40)), '--dump-dir', str(dump_dir)]
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=1500, env=env)
    return p.returncode, p.stdout + p.stderr


def png_stats(path):
    """(width, height, distinct colours in a 64x36 resize): a blank or missing picture is the failure."""
    from PIL import Image
    im = Image.open(path).convert('RGB')
    return im.size[0], im.size[1], len(set(im.resize((64, 36)).getdata()))


def last_frame_png(folder):
    files = sorted(Path(folder).glob('frame_*.png')) if Path(folder).exists() else []
    return files[-1] if files else None


def pictures_close(a, b, mean_tol=2.0, frac_tol=0.02):
    """a, b: PIL RGB images. Close = mean per-channel difference <= mean_tol (of 255) and at most frac_tol of the pixels differ by more than 16 in any channel."""
    from PIL import Image, ImageChops
    if a.size != b.size:
        return False, f'sizes {a.size} vs {b.size}'
    d = ImageChops.difference(a.convert('RGB'), b.convert('RGB'))
    px = list(d.getdata())
    mean = sum(sum(p) for p in px) / (3.0 * len(px))
    frac = sum(1 for p in px if max(p) > 16) / float(len(px))
    return mean <= mean_tol and frac <= frac_tol, f'mean diff {mean:.3f}, {100 * frac:.2f}% of pixels differ by more than 16'


def selftest_pictures():
    """The comparison must refuse a black title with a few UI bits against a real-looking one, and accept a copy with sensor-like noise."""
    from PIL import Image, ImageDraw
    import random
    rnd = random.Random(7)
    real = Image.new('RGB', (320, 180))
    px = real.load()
    for y in range(180):
        for x in range(320):
            px[x, y] = (x * 255 // 320, y * 255 // 180, 90)
    black = Image.new('RGB', (320, 180), (0, 0, 0))
    ImageDraw.Draw(black).rectangle((20, 150, 80, 170), fill=(255, 255, 255))   # a few UI bits
    ImageDraw.Draw(black).rectangle((240, 10, 300, 30), fill=(255, 255, 255))
    noisy = real.copy()
    npx = noisy.load()
    for _ in range(300):
        npx[rnd.randrange(320), rnd.randrange(180)] = (rnd.randrange(256),) * 3
    refused = not pictures_close(real, black)[0]
    accepted = pictures_close(real, noisy)[0]
    report('same.selftest (planted black title with UI bits)', refused and accepted, f'black+UI refused {refused}; a lightly noisy copy accepted {accepted}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=str(HERE.parent.parent))
    ap.add_argument('--only', default='')
    ap.add_argument('--sheets', default=str(MAIN / 'out' / 's102' / 'sheets' / 'u3'))
    args = ap.parse_args()
    root = Path(args.root).resolve()
    sys.path.insert(0, str(HERE.parent / 'fview'))
    import fview_exe as fe
    exe = Path(fe.find_or_exit(root=root, kind='dev'))
    if not exe.exists():
        print(f'FAIL  fview.exe missing: {exe}')
        return 1
    if not layer_installed():
        print(f'FAIL  {LAYER} / {VALIDATION} is not in the loader layer list (install the Vulkan SDK 1.3.250+ from LunarG)')
        return 1
    caps = {}
    for k, rel in CAPTURES.items():
        p = MAIN / rel
        if not p.exists():
            print(f'FAIL  capture missing: {p}')
            return 1
        caps[k] = p
    for p in PROFILES + ('plant_no_rgba16f', 'plant_small_push', 'plant_raised_limit'):
        if not (HERE / f'{p}.json').exists():
            print(f'FAIL  profile missing: {HERE / (p + ".json")} (run make_profiles.py)')
            return 1
    bc_decode = '--bc-decode' in (root / 'src' / 'fview' / 'main.cpp').read_text(errors='replace')   # U1 landed?
    scratch = Path(tempfile.mkdtemp(prefix='u3_cards_'))
    sheets = Path(args.sheets)
    sheets.mkdir(parents=True, exist_ok=True)
    until = (datetime.date.today() + datetime.timedelta(days=7)).isoformat()
    (sheets / 'KEEP').write_text(f'S102 lane U3: per-card picture sheets Jarrod may look at (one per simulated card)\nuntil {until}\n')
    from PIL import Image, ImageDraw
    selftest_pictures()
    pics = {}   # (profile, capture) -> png path
    for profile in [p for p in PROFILES if not args.only or p in args.only.split(',')]:
        cells = []
        for k, cap in caps.items():
            dump = scratch / f'{profile}_{k}'
            rc, text = fview(exe, root, cap, scratch, layer_env(profile, scratch), dump)
            (scratch / f'{profile}_{k}.log').write_text(text)
            m = re.search(r'validation errors (\d+)', text)
            fr = re.search(r'capture: (\d+) frames, (\d+) draws', text)
            ok_applied, why = applied(text, profile)
            order_ok = validation_above_profiles(text)
            warns = profile_warnings(text)
            png = last_frame_png(dump)
            stats = png_stats(png) if png else None
            finding = profile in KNOWN_FINDINGS and not bc_decode
            if finding:
                # A known, written-down failure (xfail): it must fail EXACTLY this way, on a card the renderer really saw as the profile's.
                pattern, what = KNOWN_FINDINGS[profile]
                hit = rc != 0 and re.search(pattern, text) is not None
                print(f'FINDING  {profile} {k}: {"as expected: " if hit else "NOT as expected: "}{what} (exit {rc})', flush=True)
                report(f'{profile} {k} (known finding, until U1)', hit and ok_applied and order_ok and not warns,
                       f'applied {ok_applied} ({why}); validation above profiles {order_ok}; layer warnings {len(warns)}; a clean run here means the fallback landed: remove the expectation')
                continue
            ok = (rc == 0 and m is not None and int(m.group(1)) == 0 and fr is not None and int(fr.group(1)) > 0 and int(fr.group(2)) > 0
                  and stats is not None and stats[2] > 8 and ok_applied and order_ok and not warns)
            extra = ''
            if bc_decode:   # U1 landed: the log says which way BC textures went
                want = 'the device CANNOT sample them; bc_decode 1 -> decoded on the CPU' if profile == 'mali_g615' else 'the device samples them'
                has = want in text and 'image_format_unsupported' not in text
                ok &= has
                extra = f'; BC line "{want}" present and no image_format_unsupported: {has}'
            report(f'{profile} {k}', ok, f'exit {rc}, validation {m.group(1) if m else "?"}, frames {fr.group(1) if fr else "?"}, draws {fr.group(2) if fr else "?"}, '
                   f'picture {stats if stats else "none written"} (w, h, colours); applied {ok_applied} ({why}); validation above profiles {order_ok}; layer warnings {warns or "none"}{extra}; '
                   f'log {scratch / (profile + "_" + k + ".log")}')
            if png:
                cells.append((k, png))
                pics[(profile, k)] = png
        if cells:
            ims = [Image.open(p).convert('RGB').resize((640, 360)) for _, p in cells]
            sheet = Image.new('RGB', (640 * len(ims), 360 + 18), (28, 28, 34))
            d = ImageDraw.Draw(sheet)
            for i, (k, _) in enumerate(cells):
                sheet.paste(ims[i], (640 * i, 18))
                d.text((640 * i + 4, 3), f'{profile}  {k}  (simulated card, validation above the profiles layer)', fill=(230, 230, 230))
            sheet.save(sheets / f'{profile}.png')
            print(f'      sheet {sheets / (profile + ".png")}')
    # same: every card's pictures against the AMD profile's, within a tolerance
    base = {k: pics.get(('amd_vega_680m', k)) for k in caps}
    for (profile, k), png in sorted(pics.items()):
        if profile == 'amd_vega_680m':
            continue
        if base.get(k) is None:
            report(f'same {profile} {k}', False, 'no AMD picture to compare with')
            continue
        ok, ev = pictures_close(Image.open(base[k]), Image.open(png))
        report(f'same {profile} {k} vs AMD', ok, ev)
    if args.only:
        print(f'logs in {scratch}')
        return 0 if all(results) else 1
    # ---- planted twins
    rc, text = fview(exe, root, caps['bryan'], scratch, layer_env('plant_no_rgba16f', scratch), None, 2)
    (scratch / 'plant_rgba16f.log').write_text(text)
    report('plant_no_rgba16f refused', rc != 0 and 'RGBA16F' in text, f'exit {rc}; floor line names RGBA16F: {"RGBA16F" in text}')
    rc, text = fview(exe, root, caps['bryan'], scratch, layer_env('plant_small_push', scratch), None, 2)
    (scratch / 'plant_push.log').write_text(text)
    m = re.search(r'validation errors (\d+)', text)
    errs = int(m.group(1)) if m else 0
    report('plant_small_push: validation sees the simulated limit', errs > 0 and validation_above_profiles(text), f'exit {rc}; validation errors {errs} (must be > 0); order ok {validation_above_profiles(text)}')
    rc, text = fview(exe, root, caps['bryan'], scratch, layer_env('plant_small_push', scratch, order=(LAYER, VALIDATION)), None, 2)
    m = re.search(r'validation errors (\d+)', text)
    report('plant_small_push, order swapped (validation BELOW profiles) shows the old blind spot', (int(m.group(1)) if m else 0) == 0 and not validation_above_profiles(text),
           f'exit {rc}; validation errors {m.group(1) if m else "?"} (0 = blind, the reason for the order); chain {chain_order(text)[:2]}')
    rc, text = fview(exe, root, caps['title'], scratch, layer_env('plant_raised_limit', scratch), None, 2)
    w = [x for x in profile_warnings(text) if 'greater than device' in x]
    report('plant_raised_limit: a raised value is caught as a layer warning', bool(w), f'exit {rc}; warnings {w[:1]}')
    rc, text = fview(exe, root, caps['title'], scratch, layer_env('intel_uhd_620', scratch, profile_name='VP_WWE_typo'), None, 2)
    ok_typo, why = applied(text, 'intel_uhd_620')
    report('applied: a misspelt profile_name is caught', not ok_typo, f'exit {rc}; {why}')
    rc, text = fview(exe, root, caps['title'], scratch, layer_env('intel_uhd_620', scratch), None, 2)   # the Intel profile judged as if it were the AMD one
    ok_swap, why = applied(text, 'amd_vega_680m')
    report('applied: one desktop card taken for the other is caught', not ok_swap, f'exit {rc}; {why}')
    if bc_decode:
        rc, text = fview(exe, root, caps['title'], scratch, layer_env('mali_g615', scratch), None, 2, extra=['--bc-decode', '0'])
        report('plant mali_g615 --bc-decode 0 re-finds the BC bug', rc != 0 and re.search(r'image_format_unsupported: format 2[4-8],', text) is not None, f'exit {rc}')
    print(f'logs in {scratch}')
    return 0 if all(results) else 1


if __name__ == '__main__':
    sys.exit(main())
