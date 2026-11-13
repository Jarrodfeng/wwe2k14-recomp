#!/usr/bin/env python3
"""Lane 131 offline proof (0 game runs): the movie planes (T_SpriteYUVAlpha, call site 0x82BC7E50) drawn by OUR quad and OUR YUV shader through the replace seam's own-draw
extension (rplay / fview --movie-ours), docs/renderer/MOVIE_OURS.md.

  py -I tools\\movie\\movie_ours_check.py [--root DIR] [--no-build] [--quick]

Inputs (a missing one FAILS, never skips): Bryan's entrance case zip (237 frames, a movie plane every frame) and the boot / menu case zip (3,329 frames, 2,871 with a movie plane), both
under out\\s101\\runs of the main checkout (the census lane's `l125_bryan` / `l125_menu` inputs); rplay.exe / fview.exe built from this tree (run here unless --no-build). No game files.

Bars (each line PASS / FAIL):
  census.<cap>      movie draws counted from the capture by call site (the capture's own draw origins): the rplay --movie-ours run's census replaced == that count, kept 0 (every
                    reason 0), the hook's own line decisions == replaced.
  picture.<cap>     rplay presented pictures, stream arm vs --movie-ours arm, at EVERY frame of Bryan and at 337 spaced frames of the menu (the first 20 movie frames, then every 9th): max channel
                    difference <= 1 level (the shader writes the same maths in another order: a pixel here and there is one level off), pixels over 1 level == 0.
  planes            every sampled movie draw: the four plane images of the draw are in slots 0..3 and the images alternate between two sets (the decoder double-buffers) [C: pcap].
  advance           frames where the movie advanced: the Y plane's content (textures.bin bytes as of that frame) differs from the previous movie frame's, and the plane images differ too.
  validation        an 8-frame fview replay with the validation layer on: errors 0.
  plants (fview, Bryan, spaced advancing frames): the clean twin matches the stream (<= 1 level), and each fault must MOVE the picture by >= FAULT_PX pixels over 1 level:
      stale_planes   the previous claimed draw's plane images (last frame's): fails on EVERY advancing frame tried
      bt601_vs_709   the wrong matrix
      no_alpha       the alpha plane ignored
      swapped_uv     Cb and Cr exchanged
  twin             the picture comparison itself can fail: one pixel changed by 3 levels in a copy of a real shot is reported as over the bar.
"""
import argparse
import os
import re
import struct
import subprocess
import sys
import time
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'particles'))

MOVIE_CALL = 0x82BC7E50
PIX = 1                 # the bar: max channel difference in levels
EXACT1_CEILING = 1e-4   # pixels differing at all (exactly 1 level) as a share of the movie quad's pixels: at most 0.01% (review F7)
FAULT_PX = 20           # a planted picture fault must move at least this many pixels (> PIX levels)
MENU_FIRST, MENU_EVERY = 20, 9
ADVANCE_BYTES = 1000    # a movie frame "advanced" when this many Y bytes changed (the two plane sets differ by 1 byte for the same picture)
CONTENT_SEC = 15        # frame section: ContentRef { u32 image, u32 pad, u64 offset, u64 size } (adapter_frame.h)


def main_checkout(here):
    try:
        common = subprocess.run(['git', '-C', here, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return here


def run(cmd, timeout=900):
    # the GPU is shared with other lanes: retry the exact start-failure message only
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

    def line(self, ok, text):
        print(('PASS  ' if ok else 'FAIL  ') + text)
        self.fails += 0 if ok else 1
        return ok


def pngs_diff(a_path, b_path):
    """-> (max channel difference, pixels over PIX, pixels differing at all)."""
    import numpy as np
    from PIL import Image
    a = np.asarray(Image.open(a_path).convert('RGB'), dtype=np.int32)
    b = np.asarray(Image.open(b_path).convert('RGB'), dtype=np.int32)
    if a.shape != b.shape:
        return 255, a.size, a.size
    d = np.abs(a - b).max(axis=2)
    return int(d.max()), int((d > PIX).sum()), int((d > 0).sum())


CENSUS = re.compile(r'\[rdr\]\[movie\] ours frames (\d+), fallback (\d+), replaced (\d+), kept (\d+) \(not ready (\d+), unjoined (\d+), mismatch (\d+), variant (\d+), texture (\d+), palette (\d+), not ours (\d+)\)')
HOOK = re.compile(r'decisions (\d+), replaced (\d+); kept: variant (\d+), geometry (\d+), quad (\d+), constants (\d+), alpha-to-coverage (\d+), planes (\d+)')


def parse_census(text):
    # (the LAST line of each kind: live-style windows log a line every 600 active frames, the final one is the whole run)
    cs, hs = CENSUS.findall(text), HOOK.findall(text)
    if not cs or not hs:
        return None
    n = [int(x) for x in cs[-1]]
    k = [int(x) for x in hs[-1]]
    return dict(ours_frames=n[0], fallback=n[1], replaced=n[2], kept=n[3], not_ready=n[4], unjoined=n[5], mismatch=n[6], variant=n[7], texture=n[8], palette=n[9], not_ours=n[10],
                decisions=k[0], hook_replaced=k[1], bad_variant=k[2], bad_geometry=k[3], bad_quad=k[4], bad_constants=k[5], bad_coverage=k[6], bad_planes=k[7])


def read_capture(capdir):
    """Per frame: the movie draws by call site (planes' image ids, in slot order) and whether the movie advanced since the last movie frame (Y content differs, plane images differ).
    Counted from the capture's own draw origins and texture contents, not from the hook."""
    import numpy as np
    import pcap
    cap = pcap.Capture(capdir)
    tex = np.memmap(os.path.join(capdir, 'textures.bin'), dtype=np.uint8, mode='r')
    content = {}       # image id -> (offset, size), as of the frame being read
    frames = []
    last_ids, last_blob = None, None
    blobs = {}
    for fi in range(len(cap.frames)):
        f = cap.frame(fi)
        if not f.has_origins:
            raise SystemExit('FAIL  %s: frame %d carries no draw origins (a version-1 capture: the movie draws cannot be named)' % (capdir, fi))
        if CONTENT_SEC in f.secs:
            es, cnt, off = f.secs[CONTENT_SEC]
            if es != 24:
                raise SystemExit('FAIL  %s: contents section element size %d, expected 24' % (capdir, es))
            for k in range(cnt):
                image, _, o, size = struct.unpack_from('<IIQQ', f.buf, off + 24 * k)
                content[image] = (o, size)
        didx, _ = f.draw_ops()
        draws = []
        areas = []
        for i in didx:
            if int(f.origins[int(i)]['call_site']) != MOVIE_CALL:
                continue
            d = f.draws[int(i)]
            slots = {int(s['slot']): int(s['image']) for s in f.slots[d['tex_first']:d['tex_first'] + d['tex_count']]}
            draws.append(tuple(slots.get(s, 0) for s in range(4)))
            vb = f.vertices(d)
            if len(vb) >= 80:
                v = np.frombuffer(bytes(vb[:80]), dtype='<f4').reshape(4, 5)
                areas.append(abs(float(v[1][0] - v[0][0]) * float(v[2][1] - v[0][1])))
            else:
                areas.append(0.0)
        advanced = False
        if draws:
            y = draws[-1][0]
            ref = content.get(y)
            blob = None
            if ref:
                blob = blobs.get(ref)
                if blob is None:
                    blobs.clear()   # (one frame's blob at a time is enough: refs repeat only within a frame pair)
                    blob = blobs.setdefault(ref, np.array(tex[ref[0]:ref[0] + ref[1]]))
            # The decoder writes the SAME picture into both plane sets, and a blob of the other set differs from this one by a pixel or two (measured: 1 byte of 512,056). The movie
            # "advanced" when the Y content changed by a real amount, and the draw's plane images changed too (so the previous frame's images hold the old picture).
            advanced = (last_ids is not None and draws[-1] != last_ids and blob is not None and last_blob is not None and blob.size == last_blob.size and
                        int((blob != last_blob).sum()) >= ADVANCE_BYTES)
            last_ids, last_blob = draws[-1], blob
        frames.append(dict(draws=draws, advanced=advanced, area=areas[-1] if areas else 0.0))
    return frames


def extract(zip_path, base):
    cap = os.path.join(base, 'capture')
    if not os.path.isfile(os.path.join(cap, 'images.bin')):
        os.makedirs(base, exist_ok=True)
        zipfile.ZipFile(zip_path).extractall(base)
    return cap


def shot_set(label, nframes, frames):
    with_movie = [i for i, f in enumerate(frames) if f['draws']]
    if label == 'bryan':
        return list(range(nframes))
    first = with_movie[:MENU_FIRST]
    rest = [i for i in with_movie[MENU_FIRST:] if i % MENU_EVERY == 0]
    return sorted(set(first + rest + [with_movie[-1]]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--quick', action='store_true', help='2 plant frames instead of 4')
    args = ap.parse_args()
    try:
        import numpy as np  # noqa: F401
        from PIL import Image  # noqa: F401
    except Exception as e:
        print('FAIL  numpy / Pillow are needed to read the pictures: %s' % e)
        return 1
    root = os.path.abspath(args.root)
    main_co = main_checkout(root)
    res = Results()
    caps = {}
    for label, run_dir in (('bryan', '20261112_122700_s100_owner_bryan_entrance'), ('menu', '20261112_122344_s100_owner_boot_menu_quit')):
        d = os.path.join(main_co, 'out', 's101', 'runs', run_dir)
        zips = [os.path.join(d, f) for f in os.listdir(d) if f.startswith('case_') and f.endswith('.zip')] if os.path.isdir(d) else []
        if not zips:
            return 0 if res.line(False, 'input missing: the %s case zip under %s' % (label, d)) else 1
        caps[label] = extract(zips[0], os.path.join(main_co, 'out', 's101', 'movie_l131', 'case_' + label))
    if not args.no_build:
        for script in ('renderer', 'fview'):
            rc, text = run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', script, 'build.ps1')], 1800)
            if rc:
                print(text[-3000:])
                res.line(False, '%s build (exit %d)' % (script, rc))
                return 1
    rplay = os.path.join(root, 'out', 'renderer', 'dev', 'rplay.exe')
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'fview')); import fview_exe as _fe   # lane 218: one locator, refuses a stale exe
    fview = _fe.find_or_exit(root=root, kind='dev')
    for exe in (rplay, fview):
        if not os.path.isfile(exe):
            res.line(False, 'no ' + exe)
            return 1
    out = os.path.join(main_co, 'out', 's101', 'movie_l131')
    os.makedirs(out, exist_ok=True)

    info = {}
    for label, cap in caps.items():
        frames = read_capture(cap)
        info[label] = frames
        total = sum(len(f['draws']) for f in frames)
        frames_with = sum(1 for f in frames if f['draws'])
        adv = [i for i, f in enumerate(frames) if f['advanced']]
        sets = {d for f in frames for d in f['draws']}
        planes_ok = total > 0 and all(0 not in d for d in sets) and len({d[0] for d in sets}) >= 2 and all(len(set(d)) == 4 for d in sets)
        res.line(planes_ok, 'planes.%s: %d movie draws in %d of %d frames; the four plane images of every draw are bound (slots 0..3, all different), %d distinct image sets (the decoder double-buffers)' % (
            label, total, frames_with, len(frames), len(sets)))
        res.line(len(adv) >= 4, 'advance.%s: the movie advanced (Y plane content and plane images both changed) at %d frames, first %s' % (label, len(adv), adv[:6]))
        shots = shot_set(label, len(frames), frames)
        arm = {}
        for tag, extra in (('stream', []), ('ours', ['--movie-ours'])):
            sd = os.path.join(out, 'rp_%s_%s' % (label, tag))
            for f in (os.listdir(sd) if os.path.isdir(sd) else []):
                os.remove(os.path.join(sd, f))
            rc, text = run([rplay, '--capture', cap, '--frames', str(len(frames)), '--no-vsync', '--shot-dir', sd, '--shots', ','.join(map(str, shots))] + extra, 1800)
            if rc:
                print(text[-2000:])
                res.line(False, '%s rplay %s arm failed (exit %d)' % (label, tag, rc))
                return 1
            arm[tag] = (sd, text)
        worst, bad_px, off_px, bad_frames, missing = 0, 0, 0, [], 0
        for f in shots:
            a, b = os.path.join(arm['stream'][0], 'frame_%04d.png' % f), os.path.join(arm['ours'][0], 'frame_%04d.png' % f)
            if not (os.path.isfile(a) and os.path.isfile(b)):
                missing += 1
                continue
            m, px, anyd = pngs_diff(a, b)
            worst, bad_px, off_px = max(worst, m), bad_px + px, off_px + anyd
            if px:
                bad_frames.append(f)
        movie_px = sum(frames[f]['area'] for f in shots)
        share = off_px / movie_px if movie_px else 1.0
        res.line(missing == 0 and worst <= PIX and bad_px == 0 and share <= EXACT1_CEILING,
                 'picture.%s: %d frames (%d with a movie plane), stream vs movie-ours: max channel difference %d levels, %d pixels over %d level (%d pixels at exactly 1 level = %.5f%% of the %d movie pixels, ceiling %.3f%%), missing shots %d%s' % (
                     label, len(shots), sum(1 for f in shots if frames[f]['draws']), worst, bad_px, PIX, off_px, 100 * share, movie_px, 100 * EXACT1_CEILING, missing, (', frames ' + str(bad_frames[:8])) if bad_frames else ''))
        c = parse_census(arm['ours'][1])
        if not c:
            print(arm['ours'][1][-1500:])
            res.line(False, 'census.%s: no movie census line in the ours arm' % label)
            continue
        ok = (c['replaced'] == total and c['decisions'] == total and c['hook_replaced'] == total and c['kept'] == 0 and c['mismatch'] == 0 and c['unjoined'] == 0 and c['variant'] == 0 and
              c['texture'] == 0 and c['not_ready'] == 0 and c['not_ours'] == 0 and c['fallback'] == 0 and
              c['bad_variant'] + c['bad_geometry'] + c['bad_quad'] + c['bad_constants'] + c['bad_coverage'] + c['bad_planes'] == 0)
        res.line(ok, 'census.%s: %d movie draws in the capture: replaced %d, hook decisions %d, kept %d (mismatch %d, variant %d, texture %d, geometry %d, quad %d, constants %d, planes %d), fallback frames %d' % (
            label, total, c['replaced'], c['decisions'], c['kept'], c['mismatch'], c['variant'], c['texture'], c['bad_geometry'], c['bad_quad'], c['bad_constants'], c['bad_planes'], c['fallback']))

    # ---------------------------------------------------------------- the comparison itself can fail ---------------------------------------------------------------------------------
    from PIL import Image
    src = os.path.join(out, 'rp_bryan_stream', 'frame_0090.png')
    twin = os.path.join(out, 'twin_frame.png')
    im = Image.open(src).convert('RGB')
    px = im.load()
    r, g, b = px[640, 500]
    px[640, 500] = (min(255, r + 3), g, b)
    im.save(twin)
    m, over, _ = pngs_diff(src, twin)
    res.line(m == 3 and over == 1, 'twin: one pixel changed by 3 levels in a copy of a real shot is reported (max %d, %d pixel over %d level): the picture comparison can fail' % (m, over, PIX))

    # ---------------------------------------------------------------- Jarrod's modes: render scale 2 (1440p), the frame pump (review F8) ----------------------------------------------------
    cap_b, nb = caps['bryan'], len(info['bryan'])
    spaced = sorted(set(list(range(0, nb, 12)) + [info['bryan'].index(next(f for f in info['bryan'] if f['advanced']))]))
    for mode, mode_args in (('scale2', ['--scale', '2']), ('pump', ['--pump', '0'])):
        pics = {}
        for tag, extra in (('stream', []), ('ours', ['--movie-ours'])):
            sd = os.path.join(out, 'rp_%s_%s' % (mode, tag))
            for fn in (os.listdir(sd) if os.path.isdir(sd) else []):
                os.remove(os.path.join(sd, fn))
            rc, text = run([rplay, '--capture', cap_b, '--frames', str(nb), '--no-vsync', '--shot-dir', sd, '--shots', ','.join(map(str, spaced))] + mode_args + extra, 1800)
            if rc:
                print(text[-2000:])
                res.line(False, 'bryan rplay %s %s arm failed (exit %d)' % (mode, tag, rc))
                return 1
            pics[tag] = (sd, text)
        worst, bad_px, off_px, missing, size = 0, 0, 0, 0, None
        for f in spaced:
            a, b = os.path.join(pics['stream'][0], 'frame_%04d.png' % f), os.path.join(pics['ours'][0], 'frame_%04d.png' % f)
            if not (os.path.isfile(a) and os.path.isfile(b)):
                missing += 1
                continue
            size = Image.open(a).size
            m, px, anyd = pngs_diff(a, b)
            worst, bad_px, off_px = max(worst, m), bad_px + px, off_px + anyd
        scale = 2.0 if mode == 'scale2' else 1.0
        movie_px = sum(info['bryan'][f]['area'] for f in spaced) * scale * scale
        share = off_px / movie_px if movie_px else 1.0
        c = parse_census(pics['ours'][1])
        res.line(missing == 0 and worst <= PIX and bad_px == 0 and share <= EXACT1_CEILING and (c is None or c['kept'] == 0),
                 'picture.bryan.%s: %d frames at %s, stream vs movie-ours: max %d levels, %d pixels over %d level (%.5f%% of the movie at exactly 1), missing %d; census %s' % (
                     mode, len(spaced), size, worst, bad_px, PIX, 100 * share, missing, ('replaced %d kept %d' % (c['replaced'], c['kept'])) if c else 'none (the pump arm may log it elsewhere)'))

    # ---------------------------------------------------------------- the UI beside it: the UI census does not change (review extra) -----------------------------------------------------
    game = os.path.join(main_co, 'game_data', 'extracted')
    if not os.path.isdir(game):
        res.line(False, 'input missing: the game files ' + game + ' (the UI coexistence arm needs the pacs)')
    else:
        ui = {}
        nm = 900
        shots_ui = [f for f in range(nm) if info['menu'][f]['draws']][::25]
        for tag, extra in (('ui', []), ('ui_movie', ['--movie-ours'])):
            sd = os.path.join(out, 'rp_menu_%s' % tag)
            for fn in (os.listdir(sd) if os.path.isdir(sd) else []):
                os.remove(os.path.join(sd, fn))
            rc, text = run([rplay, '--capture', caps['menu'], '--frames', str(nm), '--no-vsync', '--ui-ours', '--ui-text-ours', '--game', game, '--shot-dir', sd,
                            '--shots', ','.join(map(str, shots_ui))] + extra, 1800)
            if rc:
                print(text[-2000:])
                res.line(False, 'menu rplay %s arm failed (exit %d)' % (tag, rc))
                return 1
            lines = {}
            for name in ('ui', 'ui_text'):
                ms = re.findall(r'\[rdr\]\[%s\] ours frames (\d+), fallback (\d+), replaced (\d+), kept (\d+) \(([^)]*)\)' % name, text)
                lines[name] = ms[-1] if ms else None
            ui[tag] = (sd, lines, text)
        same = all(ui['ui'][1][n] is not None and ui['ui'][1][n] == ui['ui_movie'][1][n] for n in ('ui', 'ui_text'))
        worst = 0
        over_total = 0
        for f in shots_ui:
            m, px, _ = pngs_diff(os.path.join(ui['ui'][0], 'frame_%04d.png' % f), os.path.join(ui['ui_movie'][0], 'frame_%04d.png' % f))
            worst, over_total = max(worst, m), over_total + px
        cm = parse_census(ui['ui_movie'][2])
        res.line(same and worst <= PIX and over_total == 0 and cm is not None and cm['replaced'] > 0 and cm['kept'] == 0,
                 'coexist.menu: %d frames with --ui-ours --ui-text-ours (+ --movie-ours): the ui and ui_text census lines are identical in both arms (the movie draws never reach the UI): ui %s, ui_text %s; '
                 'pictures max %d levels, %d over; movie replaced %s kept %s' % (nm, ui['ui_movie'][1]['ui'], ui['ui_movie'][1]['ui_text'], worst, over_total, cm and cm['replaced'], cm and cm['kept']))

    # ---------------------------------------------------------------- validation + the plants (fview, Bryan) -----------------------------------------------------------------------------
    cap = caps['bryan']
    rc, vt = run([fview, '--capture', cap, '--movie-ours', '--frames', '8', '--validation', '1', '--replay'], 600)
    ok = rc == 0 and 'validation errors 0' in vt and 'validation active: yes' in vt
    res.line(ok, 'validation: 8-frame fview replay with the layer on, errors 0 (exit %d)' % rc)
    if not ok:
        print(vt[-1500:])
    adv = info['bryan'] and [i for i, f in enumerate(info['bryan']) if f['advanced']]
    want = 2 if args.quick else 4
    pick = [adv[(len(adv) * (k + 1)) // (want + 1)] for k in range(want)] if len(adv) >= want else adv
    print('INFO  plant frames (the movie advanced there): %s' % pick)
    cache = {}

    def arm_fv(tag, frame, extra):
        key = (tag, frame)
        if key in cache:
            return cache[key]
        d = os.path.join(out, 'fv_%s_%d' % (tag, frame))
        rc2, text = run([fview, '--capture', cap, '--frame', str(frame), '--judge-out', d] + extra, 600)
        png = os.path.join(d, 'picture.png')
        if rc2 or not os.path.isfile(png):
            print(text[-1500:])
            raise RuntimeError('fview arm %s frame %d failed (exit %d)' % (tag, frame, rc2))
        cache[key] = (png, text)
        return cache[key]

    try:
        per_frame = []
        for f in pick:
            m, over, _ = pngs_diff(arm_fv('stream', f, [])[0], arm_fv('clean', f, ['--movie-ours'])[0])
            per_frame.append((f, m, over))
        res.line(all(m <= PIX and o == 0 for _, m, o in per_frame), 'plants.clean_twin: fview clean arm vs stream at %d advancing frames: %s' % (len(pick), ', '.join('f%d max %d over %d' % t for t in per_frame)))
        area = info['bryan'][pick[0]]['area']
        for name in ('stale_planes', 'bt601_vs_709', 'no_alpha', 'swapped_uv', 'half_pixel', 'uv_flip_v', 'alpha_test_inverted'):
            moved = []
            for f in pick:
                m, over, _ = pngs_diff(arm_fv('stream', f, [])[0], arm_fv(name, f, ['--movie-ours', '--replace-fault', name])[0])
                moved.append((f, over, m))
            res.line(all(o >= FAULT_PX for _, o, _ in moved), 'plant %s: moves the picture at EVERY advancing frame tried (>= %d pixels over %d level): %s' % (
                name, FAULT_PX, PIX, ', '.join('f%d %d px (max %d)' % t for t in moved)))
        # no_alpha_test is a CONTROL, not a failing twin: the stream draw's test is alpha >= 1/255 (func 6, reference 0.0039) over a SrcAlpha blend, so a texel it would discard
        # contributes nothing either way; the picture and the target's alpha channel cannot differ (measured: 0 over 1 level in RGB, alpha channel max diff 0). The inverted twin above is what
        # proves the test is wired (reference and function reach the shader).
        ctl = [pngs_diff(arm_fv('stream', f, [])[0], arm_fv('no_alpha_test', f, ['--movie-ours', '--replace-fault', 'no_alpha_test'])[0]) for f in pick]
        res.line(all(over == 0 and m <= PIX for m, over, _ in ctl), 'control no_alpha_test: the picture does not change without the alpha test (func 6 over a src-alpha blend: invisible by construction): %s' % ', '.join(
            'f%d max %d over %d' % (f, m, o) for f, (m, o, _) in zip(pick, ctl)))
        # green_bias: +0.6/255 on the green offset moves no pixel by more than 1 level, so only the ceiling on pixels at exactly 1 level can see it (review F7)
        shares = []
        for f in pick:
            m, over, anyd = pngs_diff(arm_fv('stream', f, [])[0], arm_fv('green_bias', f, ['--movie-ours', '--replace-fault', 'green_bias'])[0])
            shares.append((f, m, anyd / area))
        res.line(all(s > EXACT1_CEILING for _, _, s in shares) and all(m <= PIX for _, m, _ in shares),
                 'plant green_bias: stays within %d level (max %s) but the exact-1-level share is over the %.3f%% ceiling at EVERY frame: %s' % (
                     PIX, '/'.join(str(m) for _, m, _ in shares), 100 * EXACT1_CEILING, ', '.join('f%d %.2f%%' % (f, 100 * s) for f, _, s in shares)))
        # the seam's own-draw contract: an own draw with count 0 / program 0 is refused by the binder (counted as mismatch), the stream's draw stays (picture == stream)
        for name in ('own_count_zero', 'own_program_zero'):
            f = pick[0]
            png, text = arm_fv(name, f, ['--movie-ours', '--replace-fault', name])
            c = parse_census(text)
            m, over, _ = pngs_diff(arm_fv('stream', f, [])[0], png)
            res.line(c is not None and c['replaced'] == 0 and c['mismatch'] > 0 and c['kept'] == c['mismatch'] and m <= PIX and over == 0,
                     'plant %s: the binder refuses the malformed own draw: replaced %s, kept %s as mismatch %s, the stream draw stays (picture vs stream max %d, %d over)' % (
                         name, c and c['replaced'], c and c['kept'], c and c['mismatch'], m, over))
    except RuntimeError as e:
        res.line(False, str(e))
    print('%s  %d failing line(s)' % ('PASS' if res.fails == 0 else 'FAIL', res.fails))
    return 1 if res.fails else 0


if __name__ == '__main__':
    sys.exit(main())
