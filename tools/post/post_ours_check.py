#!/usr/bin/env python3
r"""Lane PO offline proof (S101, 0 game runs): the post chain drawn by OUR pass through the region hook (rplay / fview --post-ours), POST_PROMOTION_MAP.md section 5, docs\renderer\POST_OURS.md.

  py -I tools\post\post_ours_check.py [--root DIR] [--no-build] [--quick] [--only core,pictures,plants,settings,noncircular,validation]

Inputs (a missing one FAILS, never skips): the three case zips of the map (Check B 341 frames, Bryan's entrance 91, wm30 423), the seven version-1 captures lane PC reads, the built
rplay.exe / fview.exe / out\tools\post_ours_core_test.exe of this tree (tools\renderer\build.ps1, tools\fview\build.ps1, tools\post\build_post_ours_test.ps1: run here unless --no-build).

Groups (each line PASS / FAIL):
  core.<cap>        the GPU-free decision (src\renderer_scene\post_ours_core.cpp) over EVERY frame of the 10 captures: shape, claimed draws and copies a frame, what stays the stream's, dead work.
                    Check B 341 x entrance 16 draws + 16 copies, Bryan 91 x two_layer 16 + 16 (stays 5: 3 restores + layer 1's 2) and 13 draws + 14 copies of dead work a frame, wm30 423 x film 4 + 3,
                    brock_tron / brock_s15_300 stay the stream's (null_input family: every chain fetch reads image 0).
  core.plant.<p>    the planted twins through the same code (fview --post-fault's checks): each fails exactly its check on every frame, the clean twin passes.
  picture.<cap>     rplay, stream arm vs --post-ours arm, the presented picture of EVERY frame: max channel difference <= 1 level (both arms run the same SPIR-V and constants: zeros are expected, and the
                    line says the comparison is circular); the census of the ours arm (ours, fallback, dropped draws / copies, dead work) exact.
  picture.scale2    the same at --scale 2 on spaced frames of the three zips (the pass's own images follow the render scale).
  noncircular       Check B's last frame, stream arm and ours arm, with the display curve, against the case zip's picture.png (what the owner renderer put on screen): ours no worse than the stream.
                    Plant no_display_curve: ours WITHOUT the curve is worse than the bar.
  settings          DEPTH OF FIELD off and FILM GRAIN off: ours equals the stream with the binder's variant swap (exact); the dof passes of a bloom-less chain are not recorded.
  plants            rplay with --post-fault: with the checks on the frame stays the stream's (census kept > 0, ours 0, picture == stream); with ,nocheck the picture moves (>= FAULT_PX pixels).
  validation        fview replay of 12 frames of each zip with the Vulkan validation layer on: errors 0.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'native'))

PIX = 1                # the bar: max channel difference in levels
FAULT_PX = 20          # a planted picture fault must move at least this many pixels (> PIX levels)
RUN_DIRS = {'brock': ('20261111_215201_s100_owner_s89_brock', 'Check B (Brock ring-in)'),
            'bryan': ('20261111_174549_s100_owner_bryan_entrance', 'Bryan entrance (two DoF layers)'),
            'wm30': ('20261111_174707_s100_owner_s90_wm30', '30 Years (film)')}
V1 = [('match300', 's100_match300'), ('s89k_a', 's100_s89k_a'), ('wm30_300', 's100_wm30_300'), ('brock_tron', 'brock_tron'), ('brock_s15_300', 'brock_s15_300'),
      ('heenan', 's100_heenan_build'), ('cards', 's100cards')]
# label -> (frames, shape, claimed draws, stays, claimed copies, dead draws, dead copies, bloom0, glare0, dof0, far_blur frames)
EXPECT = {
    'brock': (341, 'entrance', 16, 1, 16, 0, 0, 0, 0, 0, 0),
    'bryan': (91, 'two_layer', 16, 5, 16, 91 * 13, 91 * 14, 91, 91, 0, 91),
    'wm30': (423, 'film', 4, 0, 3, 0, 0, 0, 0, 0, 0),
    'match300': (3, 'match', 3, 0, 2, 0, 0, 0, 0, 0, 0),
    's89k_a': (6, 'entrance', 16, 1, 16, 0, 0, 0, 0, 0, 0),
    'wm30_300': (2, 'film', 4, 0, 3, 0, 0, 0, 0, 0, 0),
    'heenan': (100, 'editor', 16, 0, 16, 0, 0, 0, 0, 0, 0),
    'cards': (2, 'cards', 16, 2, 16, 2 * 15, 2 * 14, 2, 2, 2, 0),
}
STREAM_ONLY = {'brock_tron': 16, 'brock_s15_300': 16}   # frames; every one stays the stream's, reason null_input
CENSUS = re.compile(r'\[rdr\]\[post\] ours frames (\d+), fallback (\d+), shapes \{match (\d+), entrance (\d+), two_layer (\d+), film (\d+), editor (\d+), cards (\d+), other (\d+)\}, '
                    r'dropped draws (\d+) copies (\d+) clears (\d+) targets (\d+), dead work skipped (\d+) draws (\d+) copies \(bloom (\d+), glare (\d+), dof (\d+)\), kept (\d+) \(shape (\d+), permutation (\d+), '
                    r'slot (\d+), null_input (\d+), chain_end (\d+), filter (\d+), unscaled (\d+), texel (\d+), interleaved (\d+), reads_chain (\d+), not ready (\d+), combine (\d+), depth_clear (\d+), writes_inputs (\d+)\), passes (\d+)')
CENSUS_KEYS = ['ours', 'fallback', 'match', 'entrance', 'two_layer', 'film', 'editor', 'cards', 'other', 'draws', 'copies', 'clears', 'targets', 'dead_draws', 'dead_copies', 'bloom0', 'glare0', 'dof0', 'kept',
               'k_shape', 'k_permutation', 'k_slot', 'k_null_input', 'k_chain_end', 'k_filter', 'k_unscaled', 'k_texel', 'k_interleaved', 'k_reads_chain', 'k_not_ready', 'k_combine', 'k_depth_clear', 'k_writes_inputs', 'passes']


def main_checkout(here):
    if os.environ.get('RDR_MAIN_CHECKOUT'):
        return os.environ['RDR_MAIN_CHECKOUT']
    try:
        common = subprocess.run(['git', '-C', here, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return here


def run(cmd, timeout=900):
    # the GPU is shared with other lanes: retry the exact start-failure message only
    p = None
    for attempt in range(6):
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, errors='replace')
        text = (p.stdout or '') + (p.stderr or '')
        if p.returncode == 2 and 'Not enough graphics memory' in text:
            time.sleep(10 + 10 * attempt)
            continue
        return p.returncode, text
    return p.returncode, text


class Results:
    def __init__(self):
        self.fails = 0
        self.lines = 0

    def line(self, ok, text):
        print(('PASS  ' if ok else 'FAIL  ') + text, flush=True)
        self.fails += 0 if ok else 1
        self.lines += 1
        return ok


def parse_census(text):
    m = CENSUS.search(text)
    if not m:
        return None
    return dict(zip(CENSUS_KEYS, (int(g) for g in m.groups())))


def curve(rgb):
    """The game's display-gamma curve, Bt709Encode(SrgbDecode(c)) (tools\\post\\post_proof.py curve(), viewer_hlsl.h:788, run 6814), on uint8 RGB."""
    import numpy as np
    x = rgb.astype(np.float64) / 255.0
    lin = np.where(x <= 0.04045, x / 12.92, ((x + 0.055) / 1.055) ** 2.4)
    out = np.where(lin < 0.018, 4.5 * lin, 1.099 * np.power(np.maximum(lin, 1e-12), 0.45) - 0.099)
    return np.clip(np.round(out * 255.0), 0, 255).astype(np.int16)


def load(path):
    import numpy as np
    from PIL import Image, ImageFile
    ImageFile.LOAD_TRUNCATED_IMAGES = True
    return np.asarray(Image.open(path).convert('RGB'), dtype=np.int16)


def pngs_diff(a_path, b_path):
    import numpy as np
    a, b = load(a_path), load(b_path)
    if a.shape != b.shape:
        return 255, a.size
    d = np.abs(a - b).max(axis=2)
    return int(d.max()), int((d > PIX).sum())


def clear_dir(d):
    os.makedirs(d, exist_ok=True)
    for f in os.listdir(d):
        os.remove(os.path.join(d, f))


def core_line(text):
    """'CAP label k=v ...' -> {label: {k: v}} (draws=a..b style values kept as strings)."""
    out = {}
    for line in text.splitlines():
        if not line.startswith('CAP '):
            continue
        parts = line.split()
        d = {}
        for kv in parts[2:]:
            k, _, v = kv.partition('=')
            d[k] = v
        out[parts[1]] = d
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--quick', action='store_true', help='every 8th frame of the picture arms (development only)')
    ap.add_argument('--only', default='')
    args = ap.parse_args()
    try:
        import numpy as np  # noqa: F401
        from PIL import Image  # noqa: F401
    except Exception as e:
        print('FAIL  numpy / Pillow are needed to read the pictures: %s' % e)
        return 1
    only = set(filter(None, args.only.split(',')))

    def want(group):
        return not only or group in only
    root = os.path.abspath(args.root)
    M = main_checkout(root)
    res = Results()
    # ---- inputs ----------------------------------------------------------------------------------------------------------------------------------------------------------------
    work = os.path.join(M, 'out', 's101', 'post_ours')
    os.makedirs(work, exist_ok=True)
    caps = {}   # label -> capture folder (with geometry and textures: rplay / fview open it)
    for label, (run_dir, _what) in RUN_DIRS.items():
        d = os.path.join(M, 'out', 's101', 'runs', run_dir)
        zips = sorted(os.path.join(d, f) for f in os.listdir(d) if f.startswith('case_') and f.endswith('.zip')) if os.path.isdir(d) else []
        if not zips:
            res.line(False, 'input missing: the %s case zip under %s' % (label, d))
            return 1
        base = os.path.join(work, 'case_' + label)
        cap = os.path.join(base, 'capture')
        if not os.path.isfile(os.path.join(cap, 'images.bin')) or not os.path.isfile(os.path.join(cap, 'textures.bin')):
            os.makedirs(base, exist_ok=True)
            zipfile.ZipFile(zips[0]).extractall(base)
        caps[label] = cap
    for label, folder in V1:
        p = os.path.join(M, 'out', 'captures', folder)
        if not os.path.isdir(p):
            res.line(False, 'input missing: capture folder ' + p)
            return 1
        caps[label] = p
    if not args.no_build:
        for script in ('renderer', 'fview', 'post'):
            cmd = ['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', script, 'build_post_ours_test.ps1' if script == 'post' else 'build.ps1')]
            rc, text = run(cmd, 2400)
            if rc:
                print(text[-3000:])
                res.line(False, '%s build (exit %d)' % (script, rc))
                return 1
    rplay = os.path.join(root, 'out', 'renderer', 'dev', 'rplay.exe')
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'fview')); import fview_exe as _fe   # lane 218: one locator, refuses a stale exe
    fview = _fe.find_or_exit(root=root, kind='dev')
    core = os.path.join(root, 'out', 'tools', 'post_ours_core_test.exe')
    for exe in (rplay, fview, core):
        if not os.path.isfile(exe):
            res.line(False, 'no ' + exe + ' (build it)')
            return 1
    # the core test reads the same capture folders (frames + images.bin are enough; the three zips are the extracted ones)
    corecaps = ['%s=%s' % (k, v) for k, v in caps.items()]

    def frame_count(cap):
        return len([f for f in os.listdir(os.path.join(cap, 'frames')) if f.startswith('frame_')])
    nframes = {k: frame_count(v) for k, v in caps.items()}

    # ================================================================ core: the decision over every frame ================================================================
    if want('core'):
        rc, text = run([core] + corecaps, 900)
        got = core_line(text)
        if rc or len(got) != len(caps):
            print(text[-1500:])
            res.line(False, 'core: the decision test did not run on all %d captures (exit %d)' % (len(caps), rc))
            return 1
        for label, (frames, shape, draws, stays, copies, dd, dc, b0, g0, d0, far) in EXPECT.items():
            g = got[label]
            rng = lambda v: tuple(int(x) for x in v.split('..'))   # noqa: E731
            ok = (int(g['frames']) == frames == nframes[label] and int(g['ours']) == frames and int(g['failed']) == 0 and int(g['nochain']) == 0 and int(g.get('shape_' + shape, 0)) == frames and
                  rng(g['draws']) == (draws, draws) and rng(g['stays']) == (stays, stays) and rng(g['copies']) == (copies, copies) and int(g['dead_draws']) == dd and int(g['dead_copies']) == dc and
                  int(g['bloom0']) == b0 and int(g['glare0']) == g0 and int(g['dof0']) == d0 and int(g['far_blur_frames']) == far and int(g['depth_clears_dropped']) == 0 and int(g['copies_kept']) == 0)
            res.line(ok, 'core.%s: %d frames, every one %s: claims %d draws + %d copies a frame, %d chain draws stay the stream\'s, dead work skipped %d draws + %d copies (bloom %d, glare %d, dof %d frames), '
                     'far blur from the earlier layer on %d frames; depth / stencil clears dropped 0; kept 0%s' % (label, int(g['frames']), shape, draws, copies, stays, dd, dc, b0, g0, d0, far, '' if ok else '   [got ' + ' '.join('%s=%s' % kv for kv in g.items()) + ']'))
        for label, frames in STREAM_ONLY.items():
            g = got[label]
            ok = int(g['frames']) == frames and int(g['ours']) == 0 and int(g.get('kept_null_input', 0)) == frames
            res.line(ok, 'core.%s: all %d frames stay the stream\'s, reason null_input (the S100 07:15 / 11:39 family: every chain fetch reads image 0)' % (label, frames))
        # the planted twins through the same code (scale 2 for the scale-dependent ones)
        three = ['brock=' + caps['brock'], 'bryan=' + caps['bryan'], 'wm30=' + caps['wm30'], 'heenan=' + caps['heenan']]
        rc, text = run([core, '--scale', '2'] + three, 900)
        twin = core_line(text)
        res.line(rc == 0 and all(int(twin[k]['ours']) == int(twin[k]['frames']) for k in twin), 'core.plant.clean_twin: at --scale 2 every frame of brock / bryan / wm30 / heenan is still ours (%s)' %
                 ', '.join('%s %s/%s' % (k, twin[k]['ours'], twin[k]['frames']) for k in twin))
        plants = [('null_input', 'null_input', 'all'), ('dof_depth_filter', 'filter', 'all'), ('fixed_texel', 'texel', 'all'), ('unscaled_steps', 'unscaled', 'all'),
                  ('r32f_point', 'filter', 'linear'), ('last_pass_by_hash', 'chain_end', 'all'), ('drop_depth_clear', 'depth_clear', 'wm30'), ('writes_input_inside', 'writes_inputs', 'all'),
                  ('interleaved', 'interleaved', 'all'), ('reads_chain', 'reads_chain', 'all')]
        for name, reason, where in plants:
            rc, text = run([core, '--scale', '2', '--faults', name] + three, 900)
            got2 = core_line(text)
            bad = []
            for k, g in got2.items():
                frames = int(g['frames'])
                kept = int(g.get('kept_' + reason, 0))
                other = [kk for kk in g if kk.startswith('kept_') and kk != 'kept_' + reason]
                linear = k in ('wm30', 'heenan')   # the two of the four with a linearly filtered dof depth (no restore in the frame)
                want_kept = frames if (where == 'all' or (where == 'linear' and linear) or (where == k)) else 0
                if kept != want_kept or other or int(g['ours']) != frames - want_kept:
                    bad.append('%s kept_%s=%d (want %d) other %s ours %s' % (k, reason, kept, want_kept, other, g['ours']))
            res.line(rc == 0 and not bad, 'core.plant.%s: fails exactly the %s check on %s (the clean twin above passes)%s' % (
                name, reason, {'all': 'every frame', 'linear': 'every linear-filtered frame (wm30, heenan) and no point-filtered one', 'wm30': 'every wm30 frame (the HUD\'s depth clear) and no other'}[where], '' if not bad else '  ' + '; '.join(bad)))
        # claim variants that change the claim instead of failing a check: still ours on every frame (the picture decides, below)
        for name in ('dof_depth_address', 'inject_fail_late', 'no_combine', 'drop_out_targets'):
            rc, text = run([core, '--scale', '2', '--faults', name] + three, 900)
            g2 = core_line(text)
            res.line(rc == 0 and all(int(g2[k]['ours']) == int(g2[k]['frames']) for k in g2), 'core.plant.%s: the decision itself still claims every frame (the hook / the picture check below carries this twin)' % name)
        rc, text = run([core, '--limits', 'no_r32f_linear'] + three, 900)
        lim = core_line(text)
        res.line(rc == 0 and int(lim['wm30']['ours']) == 0 and int(lim['brock']['ours']) == int(lim['brock']['frames']) and int(lim['wm30'].get('kept_filter', 0)) == int(lim['wm30']['frames']),
                 'core.limits: a device that cannot filter R32F linearly keeps the stream\'s chain on the linear-depth frames (wm30) and still takes the point-depth ones (Check B)')

    # ================================================================ pictures: every frame, stream arm vs ours arm ========================================================================
    out = os.path.join(work, 'run')
    os.makedirs(out, exist_ok=True)

    def arm(cap, tag, frames, shots, extra, scale=1):
        sd = os.path.join(out, tag)
        clear_dir(sd)
        cmd = [rplay, '--capture', cap, '--frames', str(frames), '--no-vsync', '--shot-dir', sd, '--shots', ','.join(map(str, shots))] + (['--scale', str(scale)] if scale != 1 else []) + extra
        rc, text = run(cmd, 1800)
        if rc:
            print(text[-2000:])
            raise RuntimeError('rplay %s failed (exit %d)' % (tag, rc))
        return sd, text

    def compare(sd_a, sd_b, shots):
        worst, bad_px, bad_frames, missing = 0, 0, [], 0
        for f in shots:
            a, b = os.path.join(sd_a, 'frame_%04d.png' % f), os.path.join(sd_b, 'frame_%04d.png' % f)
            if not (os.path.isfile(a) and os.path.isfile(b)):
                missing += 1
                continue
            m, px = pngs_diff(a, b)
            worst, bad_px = max(worst, m), bad_px + px
            if px:
                bad_frames.append(f)
        return worst, bad_px, bad_frames, missing

    def dropdir(*dirs):
        for d in dirs:
            if os.path.isdir(d):
                for f in os.listdir(d):
                    os.remove(os.path.join(d, f))

    stride = 8 if args.quick else 1
    picture_caps = ['brock', 'bryan', 'wm30', 'match300', 's89k_a', 'wm30_300', 'heenan', 'cards', 'brock_tron', 'brock_s15_300']
    try:
        if want('pictures') or want('noncircular'):
            for label in picture_caps:
                n = nframes[label]
                shots = list(range(0, n, stride))
                if (n - 1) not in shots:
                    shots.append(n - 1)
                a, _ = arm(caps[label], 'p_%s_stream' % label, n, shots, [])
                b, text = arm(caps[label], 'p_%s_ours' % label, n, shots, ['--post-ours'])
                worst, bad_px, bad_frames, missing = compare(a, b, shots)
                c = parse_census(text)
                res.line(missing == 0 and worst <= PIX, 'picture.%s: %d frames (every frame%s), stream arm vs --post-ours arm: max channel difference %d levels, %d pixels over %d level, missing shots %d%s '
                         '(circular: both arms run the same SPIR-V and constants; zeros are expected)' % (label, len(shots), '' if stride == 1 else ' x %d' % stride, worst, bad_px, PIX, missing,
                                                                  (', frames ' + str(bad_frames[:8])) if bad_frames else ''))
                if not c:
                    res.line(False, 'census.%s: no [rdr][post] line in the ours arm' % label)
                    continue
                if label in EXPECT:
                    frames, shape, draws, stays, copies, dd, dc, b0, g0, d0, far = EXPECT[label]
                    ok = (c['ours'] == n and c['fallback'] == 0 and c[shape] == n and c['draws'] == draws * n and c['copies'] == copies * n and c['dead_draws'] == dd and c['dead_copies'] == dc and
                          c['bloom0'] == b0 and c['glare0'] == g0 and c['dof0'] == d0 and c['kept'] == 0 and c['passes'] > 0)
                    res.line(ok, 'census.%s: ours %d / %d frames, fallback %d, shape %s %d, dropped %d draws + %d copies (%d + %d a frame), dead work skipped %d draws %d copies (bloom %d, glare %d, dof %d), kept %d' % (
                        label, c['ours'], n, c['fallback'], shape, c[shape], c['draws'], c['copies'], draws, copies, c['dead_draws'], c['dead_copies'], c['bloom0'], c['glare0'], c['dof0'], c['kept']))
                else:
                    res.line(c['ours'] == 0 and c['fallback'] == n and c['k_null_input'] == n and c['draws'] == 0 and c['copies'] == 0, 'census.%s: the stream\'s chain stays on all %d frames (kept null_input %d), nothing dropped' % (
                        label, n, c['k_null_input']))
                if label in ('brock',):
                    keep_last = (a, b, n - 1)
                dropdir(a, b) if label != 'brock' else None
            # scale 2 on spaced frames of the three zips
            # Point-filtered depth (Check B, Bryan): exact at scale 2. REPEAT-addressed depth (30 Years, the match: the stream's dof depth sampler wraps): pixels differ at scale 2 ONLY within a few game
            # pixels of the picture's edges (wm30: 466-478 pixels over 1 level, max 3 levels, farthest 12 px from an edge at scale 2; match300: max 23 levels, edge too), because the dof taps one game texel
            # off the edge wrap to the far side in the stream and clamp in ours (post_pass.cpp). Proof it is the whole difference: --post-fault dof_depth_address (our depth slot takes the stream's REPEAT)
            # gives 0 difference. We keep clamp: the wrap is an artefact of the 360's sampler state seen at a scale where one game texel is several texels (rule 6).
            import numpy as np
            for label in ('brock', 'bryan', 'wm30', 'match300'):
                n = nframes[label]
                shots = [n - 1] if args.quick else sorted(set(list(range(0, n, 40)) + [n - 1]))
                a, _ = arm(caps[label], 's2_%s_stream' % label, n, shots, [], 2)
                b, text = arm(caps[label], 's2_%s_ours' % label, n, shots, ['--post-ours'], 2)
                worst, bad_px, bad_frames, missing = compare(a, b, shots)
                c = parse_census(text)
                linear = label in ('wm30', 'match300')
                far = 0
                if linear:
                    for f in shots:
                        d = np.abs(load(os.path.join(a, 'frame_%04d.png' % f)) - load(os.path.join(b, 'frame_%04d.png' % f))).max(axis=2)
                        ys, xs = np.nonzero(d > PIX)
                        if len(xs):
                            far = max(far, int(np.minimum.reduce([xs, d.shape[1] - 1 - xs, ys, d.shape[0] - 1 - ys]).max()))
                limit = 8 * 2   # 8 game pixels at scale 2
                ok_pic = (far <= limit) if linear else worst <= PIX
                res.line(missing == 0 and ok_pic and c is not None and c['ours'] == n and c['fallback'] == 0, 'picture.scale2.%s: %d spaced frames at --scale 2, stream vs ours: max difference %d levels, %d pixels over %d level%s; '
                         'ours %s / %d frames, fallback %s' % (label, len(shots), worst, bad_px, PIX, (' (REPEAT depth: every differing pixel within %d px of an edge, farthest %d)' % (limit, far)) if linear else ' (clamp depth: exact)',
                                                                c and c['ours'], n, c and c['fallback']))
                if linear:
                    e, _ = arm(caps[label], 's2_%s_addr' % label, n, shots, ['--post-ours', '--post-fault', 'dof_depth_address'], 2)
                    w2, px2, _bf, miss2 = compare(a, e, shots)
                    res.line(miss2 == 0 and w2 <= PIX, 'plants.dof_depth_address.%s: with the stream\'s REPEAT on our dof depth slot the scale-2 picture equals the stream: max difference %d levels, %d pixels over %d level' % (label, w2, px2, PIX))
                    dropdir(e)
                dropdir(a, b)
            # other render scales (the pass's images follow the scale; 0.5 and 0.75 round the quarter / eighth sizes up)
            for scale in (0.5, 0.75):
                for label in ('brock', 'wm30'):
                    n = min(nframes[label], 121)
                    shots = [n - 1] if args.quick else [0, 60, n - 1]
                    a, _ = arm(caps[label], 'sx_%s_stream' % label, n, shots, [], scale)
                    b, text = arm(caps[label], 'sx_%s_ours' % label, n, shots, ['--post-ours'], scale)
                    worst, bad_px, bad_frames, missing = compare(a, b, shots)
                    c = parse_census(text)
                    far = 0
                    if label == 'wm30':
                        for f in shots:
                            d = np.abs(load(os.path.join(a, 'frame_%04d.png' % f)) - load(os.path.join(b, 'frame_%04d.png' % f))).max(axis=2)
                            ys, xs = np.nonzero(d > PIX)
                            if len(xs):
                                far = max(far, int(np.minimum.reduce([xs, d.shape[1] - 1 - xs, ys, d.shape[0] - 1 - ys]).max()))
                    ok_pic = (far <= max(4, int(8 * scale))) if label == 'wm30' else worst <= PIX
                    res.line(missing == 0 and ok_pic and c is not None and c['ours'] == n and c['fallback'] == 0, 'picture.scale%g.%s: %d frames, stream vs ours: max difference %d levels, %d pixels over %d level%s; ours %s / %d' % (
                        scale, label, len(shots), worst, bad_px, PIX, (' (REPEAT depth: farthest differing pixel %d px from an edge)' % far) if label == 'wm30' else ' (exact)', c and c['ours'], n))
                    dropdir(a, b)
        # ============================================================ the non-circular bar: picture.png ==============================================================================
        if want('noncircular'):
            from_zip = os.path.join(work, 'case_brock', 'picture.png')
            a, b, last = keep_last
            sa, sb = os.path.join(a, 'frame_%04d.png' % last), os.path.join(b, 'frame_%04d.png' % last)
            import numpy as np
            import owned_verdict as OV
            P = load(from_zip)
            S, O = load(sa), load(sb)
            if not (P.shape == S.shape == O.shape):
                res.line(False, 'noncircular: picture.png %s vs shot %s: not comparable' % (P.shape, S.shape))
            else:
                def judge(img, with_curve=True):
                    x = curve(img.astype(np.uint8)) if with_curve else img
                    d = np.abs(x - P).max(axis=2)
                    return float(np.percentile(d, 99.9)), float((d > OV.TOL_SMALL).mean()), float(d.mean())
                base, mine, nocurve = judge(S), judge(O), judge(O, False)
                ok = mine[0] <= base[0] + 1 and mine[1] <= base[1] + 0.001 and mine[1] <= OV.TOL_SHARE
                res.line(ok, 'noncircular.brock (NOT independent of the stream: both arms are our renderer; this only re-tests ours against the stream through a third picture): frame %d vs the case zip\'s picture.png (what the owner renderer put on screen, display curve on): stream p99.9 %.1f share>%d %.2f%% mean %.2f; ours p99.9 %.1f share %.2f%% '
                         'mean %.2f (bar: p99.9 + 1 level, share + 0.1%%, share <= %.1f%%); the picture is a later instant than the capture\'s last frame, so the stream arm is the baseline' % (
                             last, base[0], OV.TOL_SMALL, 100 * base[1], base[2], mine[0], 100 * mine[1], mine[2], 100 * OV.TOL_SHARE))
                bad = nocurve[0] > base[0] + 1 or nocurve[1] > base[1] + 0.001
                res.line(bad, 'noncircular.plant no_display_curve: ours WITHOUT the display curve vs picture.png: p99.9 %.1f share %.2f%% mean %.2f -> over the bar (the check can fail; S100 11:44 / S101 lane 33)' % (
                    nocurve[0], 100 * nocurve[1], nocurve[2]))
            dropdir(a, b)
        # ============================================================ settings arms ================================================================================================
        if want('settings'):
            for label, n, flags in (('brock', 120, ['--depth-of-field', 'off']), ('bryan', 91, ['--depth-of-field', 'off']), ('cards', 2, ['--depth-of-field', 'off']),
                                    ('wm30', 120, ['--film-grain', 'off']), ('wm30', 120, ['--depth-of-field', 'off', '--film-grain', 'off'])):
                shots = list(range(0, n, 4 if not args.quick else 16)) + [n - 1]
                a, _ = arm(caps[label], 'st_%s_stream' % label, n, shots, flags)
                b, text = arm(caps[label], 'st_%s_ours' % label, n, shots, ['--post-ours'] + flags)
                worst, bad_px, bad_frames, missing = compare(a, b, shots)
                c = parse_census(text)
                extra = ''
                ok = c is not None and c['ours'] == n and c['fallback'] == 0
                if label == 'bryan':   # bloom 0 / 0: with DoF off nothing reads the dof chain: its passes are dead too
                    ok = ok and c['dof0'] == n
                    extra = ', dof passes not recorded on %d frames (the chain has no live bloom)' % (c and c['dof0'])
                if label == 'brock':
                    ok = ok and c['dof0'] == 0
                res.line(missing == 0 and worst <= PIX and ok, 'settings.%s %s: %d frames, ours vs the stream with the binder\'s variant swap: max difference %d levels, %d pixels over %d level; ours %s / %d%s' % (
                    label, ' '.join(flags), len(shots), worst, bad_px, PIX, c and c['ours'], n, extra))
                dropdir(a, b)
        # ============================================================ plants (pictures) ==============================================================================================
        if want('plants'):
            table = [  # name, capture, frames, shot, scale, reason (census kept_*), the picture moves with the checks off
                ('null_input', 'brock', 201, 200, 1, 'k_null_input', True), ('no_combine', 'brock', 201, 200, 1, 'k_combine', True), ('dof_depth_filter', 'brock', 201, 200, 1, 'k_filter', True),
                ('r32f_point', 'wm30', 101, 100, 1, 'k_filter', True), ('last_pass_by_hash', 'wm30', 101, 100, 1, 'k_chain_end', True),
                ('fixed_texel', 'brock', 201, 200, 2, 'k_texel', True), ('unscaled_steps', 'brock', 201, 200, 2, 'k_unscaled', True),
                ('inject_fail_late', 'brock', 201, 200, 1, 'k_not_ready', False), ('drop_depth_clear', 'wm30', 101, 100, 1, 'k_depth_clear', False),
                ('writes_input_inside', 'brock', 201, 200, 1, 'k_writes_inputs', False), ('interleaved', 'brock', 201, 200, 1, 'k_interleaved', False), ('reads_chain', 'brock', 201, 200, 1, 'k_reads_chain', False)]
            if args.quick:
                table = [t for t in table if t[1] != 'brock' or t[0] in ('null_input', 'no_combine')]
            cache = {}

            def parm(tag, cap, n, shot, scale, extra):
                key = (tag, cap, n, scale)
                if key not in cache:
                    sd, text = arm(caps[cap], 'pl_%s' % tag, n, [shot], extra, scale)
                    cache[key] = (os.path.join(sd, 'frame_%04d.png' % shot), text)
                return cache[key]
            for name, cap, n, shot, scale, reason, moves in table:
                s_png, _ = parm('stream_%s_%d' % (cap, scale), cap, n, shot, scale, [])
                c_png, _ = parm('clean_%s_%d' % (cap, scale), cap, n, shot, scale, ['--post-ours'])
                k_png, k_text = parm('%s_check' % name, cap, n, shot, scale, ['--post-ours', '--post-fault', name])
                u_png, u_text = parm('%s_nocheck' % name, cap, n, shot, scale, ['--post-ours', '--post-fault', name + ',nocheck'])
                m_clean, px_clean = pngs_diff(s_png, c_png)
                m_chk, px_chk = pngs_diff(s_png, k_png)
                m_unc, px_unc = pngs_diff(c_png, u_png)
                kc = parse_census(k_text)
                # r32f_point is driven by the device-limits flag: with the check on, the run is the "no R32F linear" device
                res.line(m_clean <= PIX, 'plants.%s clean twin (%s frame %d scale %d): ours == stream (max %d levels)' % (name, cap, shot, scale, m_clean))
                ok_chk = kc is not None and kc['ours'] == 0 and kc[reason] == n and m_chk <= PIX
                res.line(ok_chk, 'plants.%s checks ON: the frame stays the stream\'s on all %d frames (ours %s, kept[%s] %s), picture == stream (max %d levels)' % (
                    name, n, kc and kc['ours'], reason[2:], kc and kc[reason], m_chk))
                if moves:
                    res.line(px_unc >= FAULT_PX, 'plants.%s checks OFF: the picture moves by %d pixels over %d level (max %d levels) vs the clean ours (must be >= %d)' % (name, px_unc, PIX, m_unc, FAULT_PX))
                else:
                    print('INFO  plants.%s checks OFF: %d pixels over %d level (max %d levels) vs the clean ours (this plant is a claim-safety twin; its check is the bar)' % (name, px_unc, PIX, m_unc))
            if not args.quick:
                s_png, _ = parm('stream_wm30_1', 'wm30', 101, 100, 1, [])
                o_png, o_text = parm('drop_out_targets', 'wm30', 101, 100, 1, ['--post-ours', '--post-fault', 'drop_out_targets'])
                m, px = pngs_diff(s_png, o_png)
                print('INFO  plants.drop_out_targets: dropping the combine\'s own target switch moves %d pixels over %d level (max %d levels) vs the stream on wm30: NO picture signal (our last pass leaves the output bound, validation errors 0), so the `!= d.out` exception is a structural safety margin, not a proven necessity' % (px, PIX, m))
                e_png, _ = parm('stream_wm30_2', 'wm30', 3, 2, 2, [])
                a_png, a_text = parm('dof_addr', 'wm30', 3, 2, 2, ['--post-ours', '--post-fault', 'dof_depth_address'])
                m, px = pngs_diff(e_png, a_png)
                res.line(m <= PIX, 'plants.dof_depth_address (wm30 frame 2 scale 2): the stream\'s REPEAT on the dof depth slot gives max difference %d levels' % m)
            for f in os.listdir(out):
                if f.startswith('pl_'):
                    dropdir(os.path.join(out, f))
        # ============================================================ validation =====================================================================================================
        if want('validation'):
            for label in ('brock', 'bryan', 'wm30'):
                rc, vt = run([fview, '--capture', caps[label], '--post-ours', '--frames', '12', '--validation', '1', '--replay'], 900)
                c = parse_census(vt)
                ok = rc == 0 and 'validation errors 0' in vt and 'validation active: yes' in vt and c is not None and c['ours'] == 12
                res.line(ok, 'validation.%s: 12-frame fview replay with --post-ours and the layer on: errors 0, ours %s / 12 (exit %d)' % (label, c and c['ours'], rc))
                if not ok:
                    print(vt[-1500:])
    except RuntimeError as e:
        res.line(False, str(e))
    print('%s  %d failing line(s) of %d' % ('PASS' if res.fails == 0 else 'FAIL', res.fails, res.lines))
    return 1 if res.fails else 0


if __name__ == '__main__':
    sys.exit(main())
