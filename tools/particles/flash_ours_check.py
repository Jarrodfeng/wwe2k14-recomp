#!/usr/bin/env python3
"""Lane 68 offline proof (0 game runs): the crowd flashes drawn in place through the REAL replace seam (rplay / fview --flash-ours), docs/formats/crowd_flash.md.

  py -I tools\\particles\\flash_ours_check.py [--root DIR] [--no-build] [--quick]

Inputs (a missing one FAILS, never skips): Check B's case zip (arena 4, 341 frames) and Bryan's entrance case zip (91 frames), both under out\\s101\\runs of the main checkout; the game
files; rplay.exe / fview.exe built from this tree (tools\\renderer\\build.ps1 and tools\\fview\\build.ps1, run here unless --no-build).

Bars (each line PASS / FAIL):
  census.<cap>    flash draws counted from the capture by call site (pass site 0x82C139FC, the census tool's rule): the rplay run's hook replaced + not-ready == that count, the
                  not-ready draws are exactly those of frames with no scene record (frame 0: the record of frame j is j-1), mismatch / unjoined / variant / texture 0, `size unchecked` 0,
                  every draw's 20 vertices compared with the file's (verified full == replaced). Check B 1,071 draws, Bryan 125.
  picture.<cap>   rplay presented pictures, stream arm vs --flash-ours arm, at EVERY frame that holds a flash draw and at spaced frames that hold none: max channel difference <= 1 level.
  files_equal     entry 31000 of bg00 and bg04 (and bg28) by md5, and flash01's DDS payload: the model is byte-equal in the arenas, the texture too except bg28-bg30 (the evidence behind `first_arena`).
  validation      an 8-frame fview replay with the validation layer on: errors 0.
  plants (fview, Check B, the frames with the most on-screen flashes + frame 340): each must FAIL the check it targets, and the clean twin passes:
      no_depth     slot 17 (the scene-depth copy) replaced by a zero image: flashes the game hides are drawn: `picture` fails.
      all_mips     (a real bug met while building) our image keeps all 7 file mips, the game's has 3 (the slot's max_level 2): `picture` fails (max 9 levels at frame 340).
      wrong_arena  bg28's pac for arena 4: another texture, same sizes and counts: `picture` fails while every count check still passes.
      count_off    the expected index count one too many: every draw refused: `census` fails (replaced 0).
      first_arena  bg00's pac for arena 4: byte-equal to bg04's model and texture, so NOTHING can differ: stated and checked (picture == clean, files_equal), not a failing twin.
Frames and arms: rplay is one process per arm and capture (~10 s); the plants are one fview process per frame and arm."""
import argparse
import hashlib
import os
import re
import struct
import subprocess
import sys
import time
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)

FLASH_PASS = 0x82C139FC
PIX = 1                  # the bar: max channel difference in levels
NO_FLASH_SPACING = 40    # spaced control frames without a flash draw
PLANT_FRAMES_TOP = 5     # + frame 340 (the mip-clamp frame)
FAULT_PX = 20            # a planted picture fault must move at least this many pixels (> PIX levels) at some plant frame


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


CENSUS = re.compile(r'\[rdr\]\[flash\] ours frames (\d+), fallback (\d+), replaced (\d+), kept (\d+) \(not ready (\d+), unjoined (\d+), mismatch (\d+), variant (\d+), texture (\d+), palette (\d+), not ours (\d+)\)')
HOOK = re.compile(r'decisions (\d+), replaced (\d+), objects (\d+), mismatch: vertices (\d+) indices (\d+) part (\d+) content (\d+); texture: size (\d+) view (\d+), size unchecked (\d+), stream slot 0 kept (\d+); verified full (\d+) ends (\d+)')


def parse_census(text):
    c, h = CENSUS.search(text), HOOK.search(text)
    if not c or not h:
        return None
    n = [int(x) for x in c.groups()]
    k = [int(x) for x in h.groups()]
    return dict(ours_frames=n[0], fallback=n[1], replaced=n[2], kept=n[3], not_ready=n[4], unjoined=n[5], mismatch=n[6], variant=n[7], texture=n[8], palette=n[9], not_ours=n[10],
                decisions=k[0], hook_replaced=k[1], objects=k[2], bad_vertices=k[3], bad_indices=k[4], bad_part=k[5], bad_content=k[6], bad_size=k[7], bad_view=k[8],
                size_unchecked=k[9], slot0_kept=k[10], verified_full=k[11], verified_ends=k[12])


def flash_frames(capdir):
    """Per frame: flash draws (pass site 0x82C139FC) and the ones whose centre is on screen (clip = ViewProj rows c56-c59 x (c214.xyz, 1)). Counted from the capture's draw origins."""
    import numpy as np
    import pcap
    cap = pcap.Capture(capdir)
    out = []
    for fi in range(len(cap.frames)):
        f = cap.frame(fi)
        if not f.has_origins:
            raise SystemExit('FAIL  %s: frame %d carries no draw origins (a version-1 capture: the flashes cannot be named)' % (capdir, fi))
        didx = f.draw_ops()[0]
        n_draws = on_screen = 0
        for i in didx:
            if int(f.origins[int(i)]['pass_site']) != FLASH_PASS:
                continue
            n_draws += 1
            d = f.draws[int(i)]
            fv = f.bank(int(d['vs_bank'])).view('<f4')
            vp = fv[4 * 56:4 * 60].reshape(4, 4)
            clip = vp @ np.append(fv[4 * 214:4 * 214 + 3], np.float32(1.0))
            cx, cy, cz, cw = (float(v) for v in clip)
            if cw > 0 and abs(cx) <= cw and abs(cy) <= cw and 0 <= cz <= cw:
                on_screen += 1
        out.append((n_draws, on_screen))
    return out


def pngs_diff(a_path, b_path):
    import numpy as np
    from PIL import Image
    a = np.asarray(Image.open(a_path).convert('RGB'), dtype=np.int32)
    b = np.asarray(Image.open(b_path).convert('RGB'), dtype=np.int32)
    if a.shape != b.shape:
        return 255, a.size
    d = np.abs(a - b).max(axis=2)
    return int(d.max()), int((d > PIX).sum())


def files_equal_evidence(game, res):
    """bg00 / bg04 / bg28: entry 31000 md5 and flash01 payload md5, read with the repo's own pac reader (tools\\wm30\\pac.py)."""
    sys.path.insert(0, os.path.join(ROOT, 'tools', 'wm30'))
    import pac

    def facts(n):
        data = pac.load(os.path.join(game, 'pac', 'bg', 'bg%02d.pac' % n))
        ents = dict(pac.pach(data, 0x4000))
        if 31000 not in ents:
            raise SystemExit('FAIL  bg%02d.pac has no entry 31000' % n)
        model = hashlib.md5(pac.decode(ents[31000])).hexdigest()[:10]
        d = pac.decode(ents[400])
        cnt = struct.unpack_from('<I', d, 0)[0]
        tex = None
        for i in range(cnt):
            r = 0x10 + 32 * i
            if d[r:r + 16].split(b'\0')[0].decode().lower() == 'flash01':
                size, off = struct.unpack_from('<II', d, r + 20)
                tex = hashlib.md5(d[off:off + size]).hexdigest()[:10]
        if tex is None:
            raise SystemExit('FAIL  bg%02d.pac directory 400 has no flash01' % n)
        return model, tex
    f0, f4, f28 = facts(0), facts(4), facts(28)
    res.line(f0 == f4, 'files_equal: bg00 and bg04 hold the same flash model (md5 %s) and texture (md5 %s) -> `first_arena` cannot differ' % (f0[0], f0[1]) if f0 == f4
             else 'files_equal: bg00 %s differs from bg04 %s' % (f0, f4))
    res.line(f28[0] == f4[0] and f28[1] != f4[1], 'files_equal (positive control): bg28 has the same model but another texture (%s vs %s) -> `wrong_arena` moves the picture, count checks cannot see it' % (f28[1], f4[1])
             if f28[0] == f4[0] and f28[1] != f4[1] else 'files_equal control: bg28 %s vs bg04 %s' % (f28, f4))
    # the twin: the equality check must be able to fail
    twin_ok = facts(28) != facts(4)
    res.line(twin_ok, 'files_equal planted twin: comparing bg28 with bg04 is reported as DIFFERENT (the check can fail)')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--quick', action='store_true', help='plants on 2 frames instead of 6')
    args = ap.parse_args()
    try:
        import numpy as np  # noqa: F401
        from PIL import Image  # noqa: F401
    except Exception as e:
        print('FAIL  numpy / Pillow are needed to read the pictures: %s' % e)
        return 1
    root = os.path.abspath(args.root)
    main_co = main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    res = Results()
    caps = {}
    for label, run_dir in (('brock', '20261111_215201_s100_owner_s89_brock'), ('bryan', '20261111_174549_s100_owner_bryan_entrance')):
        d = os.path.join(main_co, 'out', 's101', 'runs', run_dir)
        zips = [os.path.join(d, f) for f in os.listdir(d) if f.startswith('case_') and f.endswith('.zip')] if os.path.isdir(d) else []
        if not zips:
            return 0 if res.line(False, 'input missing: the %s case zip under %s' % (label, d)) else 1
        base = os.path.join(main_co, 'out', 's101', 'arena', 'case_brock') if label == 'brock' else os.path.join(main_co, 'out', 's101', 'flash_l68', 'case_' + label)
        cap = os.path.join(base, 'capture')
        if not os.path.isfile(os.path.join(cap, 'images.bin')):
            os.makedirs(base, exist_ok=True)
            zipfile.ZipFile(zips[0]).extractall(base)
        if label == 'brock' and not os.path.isdir(os.path.join(cap, 'scene')):
            res.line(False, 'the %s capture has no scene\\ records: %s' % (label, cap))
            return 1
        caps[label] = cap
    if not os.path.isdir(game):
        res.line(False, 'input missing: the game files ' + game)
        return 1
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
    out = os.path.join(main_co, 'out', 's101', 'flash_l68')
    os.makedirs(out, exist_ok=True)

    # ---------------------------------------------------------------- census + picture, both captures (rplay) ------------------------------------------------------------------
    info = {}
    for label, cap in caps.items():
        per = flash_frames(cap)
        info[label] = per
        total = sum(n for n, _ in per)
        frames_with = [i for i, (n, _) in enumerate(per) if n]
        controls = [i for i, (n, _) in enumerate(per) if not n and i % NO_FLASH_SPACING == 0]
        shots = sorted(set(frames_with + controls))
        print('INFO  %s: %d frames, %d flash draws in %d frames (%d on screen), %d no-flash control frames' % (label, len(per), total, len(frames_with), sum(o for _, o in per), len(controls)))
        if label == 'bryan':
            # Bryan's zip was taken without --wwe2k14_scene_records (no scene\ folder, no asset names): the record names the arena, so the hook cannot run on it. The count is
            # the stream's (lane 65: 125); the refusal must be loud (rplay exits 2 and says why), never a silent stream replay that "passes".
            res.line(total == 125, 'census.bryan: the capture holds 125 flash draws in %d frames (lane 65); the hook is NOT run on it: no scene records (arena unknown)' % len(frames_with))
            rc, text = run([rplay, '--capture', cap, '--frames', '4', '--no-vsync', '--flash-ours', '--game', game], 300)
            res.line(rc == 2 and 'no scene records' in text, 'census.bryan refusal: rplay --flash-ours on a capture without scene records exits %d with "%s"' % (
                rc, 'the capture has no scene records' if 'no scene records' in text else text[-200:].replace('\n', ' ')))
            continue
        arm = {}
        for tag, extra in (('stream', []), ('ours', ['--flash-ours', '--game', game])):
            sd = os.path.join(out, 'rp_%s_%s' % (label, tag))
            for f in (os.listdir(sd) if os.path.isdir(sd) else []):
                os.remove(os.path.join(sd, f))
            rc, text = run([rplay, '--capture', cap, '--frames', str(len(per)), '--no-vsync', '--shot-dir', sd, '--shots', ','.join(map(str, shots))] + extra, 900)
            if rc:
                print(text[-2000:])
                res.line(False, '%s rplay %s arm failed (exit %d)' % (label, tag, rc))
                return 1
            arm[tag] = (sd, text)
        worst, bad_px, bad_frames, missing = 0, 0, [], 0
        for f in shots:
            a, b = os.path.join(arm['stream'][0], 'frame_%04d.png' % f), os.path.join(arm['ours'][0], 'frame_%04d.png' % f)
            if not (os.path.isfile(a) and os.path.isfile(b)):
                missing += 1
                continue
            m, px = pngs_diff(a, b)
            worst, bad_px = max(worst, m), bad_px + px
            if px:
                bad_frames.append(f)
        res.line(missing == 0 and worst <= PIX,
                 'picture.%s: %d frames (%d with flashes), stream vs flash-ours: max channel difference %d levels, %d pixels over %d level, missing shots %d%s' % (
                     label, len(shots), len(frames_with), worst, bad_px, PIX, missing, (', frames ' + str(bad_frames[:8])) if bad_frames else ''))
        c = parse_census(arm['ours'][1])
        if not c:
            print(arm['ours'][1][-1500:])
            res.line(False, 'census.%s: no flash census line in the ours arm' % label)
            continue
        unready = per[0][0]   # frame 0 has no scene record (record j-1)
        ok = (c['replaced'] + c['not_ready'] == total and c['not_ready'] == unready and c['replaced'] == total - unready and c['mismatch'] == 0 and c['unjoined'] == 0 and c['variant'] == 0 and
              c['texture'] == 0 and c['bad_vertices'] + c['bad_indices'] + c['bad_part'] + c['bad_content'] == 0 and c['size_unchecked'] == 0 and c['verified_full'] == c['replaced'] and
              c['objects'] == 1)
        res.line(ok, 'census.%s: %d flash draws in the capture: replaced %d + not ready %d (frame 0 holds %d, no record yet), mismatch %d, unjoined %d, variant %d, texture %d, content %d, size unchecked %d, '
                 'verified %d vertices-sets, objects %d' % (label, total, c['replaced'], c['not_ready'], unready, c['mismatch'], c['unjoined'], c['variant'], c['texture'], c['bad_content'],
                                                              c['size_unchecked'], c['verified_full'], c['objects']))
        if label == 'brock':
            res.line(total == 1071, 'census.brock: the capture holds 1,071 flash draws (the map / lane 65 number)')
        else:
            res.line(total == 125, 'census.bryan: the capture holds 125 flash draws (lane 65 number)')

    # ---------------------------------------------------------------- the files behind first_arena / wrong_arena --------------------------------------------------------------
    files_equal_evidence(game, res)

    # ---------------------------------------------------------------- validation + the plants (fview, Check B) ----------------------------------------------------------------------
    cap = caps['brock']
    rc, vt = run([fview, '--capture', cap, '--flash-ours', '--game', game, '--frames', '8', '--validation', '1', '--replay'], 600)
    ok = rc == 0 and 'validation errors 0' in vt and 'validation active: yes' in vt
    res.line(ok, 'validation: 8-frame fview replay with the layer on, errors 0 (exit %d)' % rc)
    if not ok:
        print(vt[-1500:])
    per = info['brock']
    ranked = sorted(range(len(per)), key=lambda i: (-per[i][1], i))
    top = [f for f in ranked if per[f][1] > 0][:2 if args.quick else PLANT_FRAMES_TOP]
    plant_frames = sorted(set(top + [340]))
    print('INFO  plant frames (most on-screen flashes + 340): %s' % plant_frames)
    cache = {}

    def arm(tag, frame, extra):
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

    def moved(tag, extra, ref_tag):
        total_px, worst = 0, 0
        for f in plant_frames:
            m, px = pngs_diff(arm(ref_tag, f, [] if ref_tag == 'stream' else ['--flash-ours', '--game', game])[0], arm(tag, f, extra)[0])
            total_px, worst = total_px + px, max(worst, m)
        return total_px, worst

    clean = ['--flash-ours', '--game', game]
    try:
        px, worst = moved('clean', clean, 'stream')
        res.line(px == 0 and worst <= PIX, 'plants.clean_twin: fview clean arm vs stream at %d frames: %d pixels over %d level (max %d)' % (len(plant_frames), px, PIX, worst))
        for name in ('no_depth', 'all_mips', 'wrong_arena'):
            px, worst = moved(name, clean + ['--replace-fault', name], 'stream')
            c = parse_census(arm(name, plant_frames[-1], clean + ['--replace-fault', name])[1])
            note = ''
            if name == 'wrong_arena' and c:
                note = '; count checks unchanged: mismatch %d, replaced %d' % (c['mismatch'], c['replaced'])
            res.line(px >= FAULT_PX, 'plant %s: moves the picture by %d pixels over %d level (max %d levels) vs the stream (must be >= %d)%s' % (name, px, PIX, worst, FAULT_PX, note))
        c = parse_census(arm('count_off', plant_frames[-1], clean + ['--replace-fault', 'count_off'])[1])
        res.line(c is not None and c['replaced'] == 0 and c['mismatch'] > 0 and c['bad_indices'] > 0,
                 'plant count_off: the census check fails: replaced %s, mismatch %s (indices %s) -- every draw refused' % (c and c['replaced'], c and c['mismatch'], c and c['bad_indices']))
        px, worst = moved('first_arena', clean + ['--replace-fault', 'first_arena'], 'clean')
        res.line(px == 0 and worst == 0, 'control first_arena: bg00 for arena 4 gives the SAME picture as the clean arm (%d pixels differ): the model and texture are byte-equal (files_equal above), so this twin cannot fail' % px)
    except RuntimeError as e:
        res.line(False, str(e))
    print('%s  %d failing line(s)' % ('PASS' if res.fails == 0 else 'FAIL', res.fails))
    return 1 if res.fails else 0


if __name__ == '__main__':
    sys.exit(main())
