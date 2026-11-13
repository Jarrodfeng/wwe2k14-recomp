r"""Lane 123 (S101, SH2) gate driver: our shadow passes (src\renderer_adapter\shadows_ours.h, docs\renderer\SHADOWS_OURS.md). 0 game runs.

  py -I tools\shadow\shadows_ours_check.py clean512     fview --shadows-check (this checkout's out\renderer\dev\fview.exe, tools\fview\build.ps1) plays Check B (brock), Bryan and wm30
                                                         with every frame in lockstep: A = the stream, B = our shadow passes at 512. The readers' images (the destinations of the copies out
                                                         of the shadow targets) equal the stream's within rounding and the presented picture is byte-equal on every frame; validation 0.
  py -I tools\shadow\shadows_ours_check.py size1024     Check B at 1024: the picture within the owned_check-style tolerance, every map exactly twice the stream's on both axes (no 16:9
                                                         stretch), the map's footprint where the stream's is.
  py -I tools\shadow\shadows_ours_check.py plants       each planted fault must FAIL its check: R scaled_map, R copy_swap, R blur_input_null, R floor_reads_wrestler_map, R p_slots_cast,
                                                         rim_as_shadow (the rim light map counted as a shadow)
  py -I tools\shadow\shadows_ours_check.py all

Captures: the extracted capture folders of the three S100 owner case zips (out\lane86\<name>\capture in the main checkout, made by tools\shadow\shadow_roles.py). A missing input FAILs."""
import json
import os
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def main_checkout():
    if os.environ.get('RDR_MAIN_CHECKOUT'):
        return Path(os.environ['RDR_MAIN_CHECKOUT'])
    try:
        common = subprocess.run(['git', '-C', str(ROOT), 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return Path(common).parent
    except Exception:
        pass
    return ROOT


MAIN = main_checkout()
import sys as _sys
_sys.path.insert(0, str(ROOT / 'tools' / 'fview'))   # lane 218: the shared locator (refuses an exe older than the renderer sources)
import fview_exe as fe
try:
    FVIEW = Path(fe.find_fview(root=ROOT, kind='dev'))
except fe.FviewError as _e:
    print('FAIL  ' + str(_e))
    _sys.exit(1)
OUT = ROOT / 'out' / 'l123'
CAPS = {n: MAIN / 'out' / 'lane86' / n / 'capture' for n in ('brock', 'bryan', 'wm30')}
fails = []


def fail(msg):
    fails.append(msg)
    print('FAIL  ' + msg)


def ok(msg):
    print('ok    ' + msg)


def run(cap, label, extra, tries=8):
    """One fview --shadows-check run; the GPU is shared, so only the 'Not enough graphics memory' message is retried."""
    if not FVIEW.exists():
        fail('%s does not exist (tools\\fview\\build.ps1)' % FVIEW)
        return None
    folder = CAPS[cap]
    if not (folder / 'images.bin').exists():
        fail('cannot read %s (run tools\\shadow\\shadow_roles.py check first: a check that reads nothing must fail)' % folder)
        return None
    OUT.mkdir(parents=True, exist_ok=True)
    js = OUT / (label + '.json')
    if js.exists():
        js.unlink()
    cmd = [str(FVIEW), '--capture', str(folder), '--root', str(ROOT), '--validation', '1', '--shadows-check', str(js)] + extra
    for attempt in range(tries):
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=1800)
        text = r.stdout + r.stderr
        if 'Not enough graphics memory' in text:
            print('      %s: GPU busy (attempt %d), retrying' % (label, attempt + 1))
            time.sleep(20)
            continue
        if not js.exists():
            fail('%s: fview wrote no json (exit %d): %s' % (label, r.returncode, text.strip().splitlines()[-1:] ))
            return None
        data = json.loads(js.read_text())
        data['exit'] = r.returncode
        return data
    fail('%s: the GPU stayed busy' % label)
    return None


EXPECTED_IMAGES = {'brock': 8, 'bryan': 8, 'wm30': 5}  # the destinations of the copies out of the shadow targets (214 215 216 217 220 221) + the reductions (218 219); wm30: 733 734 736 + reductions
MIN_BLUR_BAR = {'brock': 0.8, 'bryan': 0.8, 'wm30': 4.5}  # mean of the final min-blurred map's rectangles against the stream (clean: 0.50 / 0.53 / 3.81); plant dup_no_invalidate gives 3.7 on Check B
MINBLUR_MAP = {'brock': '214', 'bryan': '214', 'wm30': '733'}
PROPS_MAP = {'brock': '217', 'bryan': '217', 'wm30': '736'}  # the props map's copy (no min blur): the half-texel bar reads it


def judge(d, size, plane, cap_name, scale=1):
    """The problems (empty = clean). size = the map edge, plane = the plane chain's factor against the stream's."""
    bad = []
    if d['exit'] != 0:
        bad.append('exit %d / validation errors %d' % (d['exit'], d['validation_errors']))
    if d['compared'] < 1 or d['classified_frames'] < d['frames']:
        bad.append('shadow images found on %d of %d frames' % (d['classified_frames'], d['frames']))
    if d['no_dest']:
        bad.append('%d reader images unreadable' % d['no_dest'])
    if 'refused 0' not in d['census'] or 'readers 0,' in d['census']:
        bad.append('census: ' + d['census'])
    if len(d['images']) != EXPECTED_IMAGES[cap_name]:
        bad.append('%d images compared, expected %d (an image that is not a shadow image was classified)' % (len(d['images']), EXPECTED_IMAGES[cap_name]))
    pic = d['picture']
    if pic['frames'] < 1:
        bad.append('no picture compared')
    if pic['shape_fail']:
        bad.append('picture shape differs')
    for ident, im in d['images'].items():
        if im['shape_fail']:
            bad.append('image %s: shape (stream %s, ours %s)' % (ident, im['a'], im['b']))
        k = {1: size / 512.0, 2: plane}.get(im.get('chain'), 1.0)  # the reductions (chain 0) keep the stream's size
        aw, ah = [int(v) for v in im['a'].split('x')]
        want = '%dx%d' % (round(aw * k), round(ah * k))
        if im['b'] != want:
            bad.append('image %s: ours %s, wanted %s (stretched or not scaled)' % (ident, im['b'], want))
        if im['max_bbox_err'] > 0.03 and ident != MINBLUR_MAP[cap_name]:  # (the final min-blurred map's corner texel is not always the clear colour: its footprint is judged by the mean, below)
            bad.append('image %s: footprint off by %.3f of the map' % (ident, im['max_bbox_err']))
        if size == 512 and plane == 1.0:
            if im['max_diff'] > 1 and im['max_mean'] > 0.01:  # the final min-blurred map: texels beside the stream's partial rectangles (SHADOWS_OURS.md 5)
                if im['max_mean'] > MIN_BLUR_BAR[cap_name]:
                    bad.append('image %s: mean diff %.3f (bar %.2f)' % (ident, im['max_mean'], MIN_BLUR_BAR[cap_name]))
            elif im['max_diff'] > 1:
                bad.append('image %s: max diff %.0f' % (ident, im['max_diff']))
        elif im.get('chain') == 1 and ident == PROPS_MAP[cap_name] and im['max_mean'] > 0.022:
            bad.append('image %s: mean inside the rectangles %.4f over the bar 0.022 (a half-texel slide shows here)' % (ident, im['max_mean']))
    if size == 512 and plane == 1.0 and scale == 1:
        if pic['max_diff'] != 0:
            bad.append('picture differs from the stream (max %.0f, mean %.5f)' % (pic['max_diff'], pic['max_mean']))
    elif pic['max_mean'] > (0.0013 if plane == 1.0 else 0.01):  # a bigger plane mask moves the ring mat's shadow edge by sub-texels (the shadow_blur's 1/512 constants)
        bad.append('picture mean diff %.5f over the bar' % pic['max_mean'])
    return bad


ARMS = [  # (label, capture, size, plane percent, scale, frames)
    ('brock_512', 'brock', 512, 100, 1, 100000), ('bryan_512', 'bryan', 512, 100, 1, 100000), ('wm30_512', 'wm30', 512, 100, 1, 100000),
]
SIZE_ARMS = [('brock_256', 256, 100, 1), ('brock_1024', 1024, 100, 1), ('brock_2048', 2048, 100, 1), ('brock_plane200', 512, 200, 1), ('brock_plane0_scale2', 512, 0, 2)]


def args_of(size, pct, scale, frames, every, extra=()):
    a = ['--frames', str(frames), '--check-every', str(every), '--shadow-map-size', str(size), '--shadow-plane-scale', str(pct)]
    if scale != 1:
        a += ['--scale', str(scale)]
    return a + list(extra)


def plane_factor(pct, scale):
    return (round(scale) if pct == 0 else pct / 100.0)


def clean512():
    for label, cap, size, pct, scale, frames in ARMS:
        d = run(cap, label, args_of(size, pct, scale, frames, 1))
        if d is None:
            continue
        bad = judge(d, size, 1.0, cap)
        if bad:
            for b in bad:
                fail('%s at 512: %s' % (cap, b))
        else:
            ok('%s at 512: %d frames, %d compared, %d images (reader images + reductions), picture byte-equal on every compared frame, validation 0' % (cap, d['frames'], d['compared'], len(d['images'])))


def size1024():
    for label, size, pct, scale in SIZE_ARMS:
        d = run('brock', label, args_of(size, pct, scale, 120, 4))
        if d is None:
            continue
        bad = judge(d, size, plane_factor(pct, scale), 'brock', scale)
        if bad:
            for b in bad:
                fail('%s: %s' % (label, b))
        else:
            ok('%s: %d images at the wanted sizes, footprints match, picture mean diff %.4f of 255, props map mean %.4f, validation 0' % (label, len(d['images']), d['picture']['max_mean'], d['images'][PROPS_MAP['brock']]['max_mean']))


# (name, map size, plane percent, scale); every plant has a control with the SAME arguments but the fault `none` (or keying `clean`)
PLANTS = [('scaled_map', 1024, 100, 1), ('copy_swap', 512, 100, 1), ('blur_input_null', 512, 100, 1), ('floor_reads_wrestler_map', 512, 100, 1),
          ('half_texel_rescaled', 1024, 100, 1), ('dup_no_invalidate', 512, 100, 1), ('no_viewport_scale', 1024, 100, 1), ('plane_unscaled_chain', 512, 200, 1),
          ('p_slots_cast', 512, 100, 1), ('rim_as_shadow', 512, 100, 1)]


def plants():
    for name, size, pct, scale in PLANTS:
        rim = name == 'rim_as_shadow'
        fault = ['--shadow-keying', 'rim_as_shadow'] if rim else ['--shadows-ours-fault', name]
        control = ['--shadow-keying', 'clean'] if rim else ['--shadows-ours-fault', 'none']
        dc = run('brock', 'control_' + name, args_of(size, pct, scale, 40, 4, control))
        if dc is not None:
            badc = judge(dc, size, plane_factor(pct, scale), 'brock', scale)
            if badc:
                fail('control for %s (fault none, same arguments) is not clean: %s' % (name, badc[0]))
                continue
        d = run('brock', 'plant_' + name, args_of(size, pct, scale, 40, 4, fault))
        if d is None:
            continue  # (run() already recorded the FAIL: a plant that cannot run proves nothing)
        bad = judge(d, size, plane_factor(pct, scale), 'brock', scale)
        if bad:
            ok('plant %s FAILS as it must (%s); its control is clean' % (name, bad[0]))
        else:
            fail('plant %s passed: the check cannot see this fault' % name)


def main():
    what = sys.argv[1] if len(sys.argv) > 1 else 'all'
    if what in ('clean512', 'all'):
        clean512()
    if what in ('size1024', 'all'):
        size1024()
    if what in ('plants', 'all'):
        plants()
    if what not in ('clean512', 'size1024', 'plants', 'all'):
        print(__doc__)
        return 2
    print('\n%d failure(s)' % len(fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
