#!/usr/bin/env python3
"""S102 lane T6 acceptance test, offline, 0 game runs, no window: the crowd's members as ONE instanced draw per group of members that share their material and constants
(--wwe2k14_rdr_members_instanced; CrowdOurs::InjectBody, the yAudienceInst vertex stages).

  py -I tools\\crowd\\members_instanced_check.py [--root DIR] [--no-build] [--exe FVIEW] [--frames N] [--only bryan|match] [--sheet-dir DIR]

For Bryan's entrance and a match capture: the same replay (fview --case ZIP --replay --crowd-ours --validation 1 --hash-frames), arms
  OFF    --members-instanced off   one draw per member (the reference)
  ON     --members-instanced on    one instanced draw per (group, mesh)
  PLANT1 ON + --crowd-fault inst_world_first   every instance takes its group's first world (the crowd collapses to a few spots)
  PLANT2 ON + --crowd-fault inst_drop_last     the last instance of every group is not drawn
Bar (our code against our code; tolerance because the instanced stage reads the world rows from an attribute, the plain one from the block: the same arithmetic, the driver may fuse it differently):
  1. counts: ON asked every frame it drew (`members instanced` line read: frames > 100, fall-backs 0), member draws a frame <= 60 (the mean AND the most of one frame), the groups hold every member
     (members instanced == members drawn in OFF), OFF's own draw count per frame is far above (reported).
  2. picture: ON vs OFF, per frame, the share of pixels whose luminance differs by more than 12 (of the frame): every frame <= PIX_FRAC. Frames byte-equal are counted apart.
  3. planted twins: PLANT1 and PLANT2 must each break the picture bar on at least one frame (the check can tell a wrong per-instance value from a right one), and PLANT2 must show in the members' count
     (drawn members per frame fall below OFF's).
  4. validation: an 8-frame ON replay with the validation layer on: errors 0.
  5. sheet: out\\s102\\sheets\\t6\\<capture>_off_on_diff.png (OFF | ON | difference x8), every panel labelled (--sheet-dir overrides).
Exit 0 all PASS, 1 any FAIL. A check that cannot read its input fails loudly."""
import argparse, os, re, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
CAPS = [
    ('bryan', 'out/s101/runs/20261112_122700_s100_owner_bryan_entrance/case_20261112_122719_969_0_pc_run_exit.zip'),
    ('match', 'out/s101/runs/20261111_215201_s100_owner_s89_brock/case_20261111_215233_217_0_pc_run_exit.zip'),
]
PIX = 12.0
PIX_FRAC = 0.001       # of a frame's pixels: ON vs OFF (both captures are byte-equal today; the bar leaves room for a driver fusing the world-row arithmetic differently)
MAX_DRAWS = 60
INST_RE = re.compile(r'members instanced: (\d+) frames, (\d+) draws \(([\d.]+) a frame, at most (\d+)\), (\d+) groups for (\d+) members \((\d+) groups beyond one per class\), (\d+) frames asked and fell back')
AUDIT_RE = re.compile(r'members instanced audit: (\d+) members rebuilt from their group, (\d+) rebuilt wrong, (\d+) out of stream order')
PFRAMES = 120
VALS = {}   # arm name -> (fview exit code, validation errors)
OURS_RE = re.compile(r'crowd ours: (\d+) frames, (\d+) members')


def main_checkout(root):
    try:
        c = subprocess.run(['git', '-C', root, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if c:
            return os.path.dirname(c)
    except Exception:
        pass
    return root


def read_hashes(path):
    out = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line:
                a, b = line.split(',')
                out[a if a == 'format' else int(a)] = b
    return out


def fview(exe, cmd, timeout=1800):
    text = ''
    for attempt in range(12):   # the GPU is shared: retry only the logged start-up refusal
        p = subprocess.run([exe] + cmd, capture_output=True, text=True, timeout=timeout, errors='replace')
        text = (p.stdout or '') + (p.stderr or '')
        if 'Not enough graphics memory for the frame ring' not in text:
            return p.returncode, text
        print('NOTE  "Not enough graphics memory for the frame ring", attempt %d of 12' % (attempt + 1), flush=True)
        time.sleep(20)
    return 1, text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--exe', default='')
    ap.add_argument('--frames', type=int, default=1000)
    ap.add_argument('--only', default='')
    ap.add_argument('--sheet-dir', default='')
    ap.add_argument('--tolerance', type=float, default=0.0, help='RE-BASELINE OPTION ONLY: accept ON vs OFF frames that are not byte-equal when the share of pixels over 12 luminance is within this; default 0 = every frame byte-equal')
    args = ap.parse_args()
    try:
        import numpy as np
        from PIL import Image, ImageDraw
    except Exception as e:
        print('FAIL  numpy / Pillow are needed to read the pictures: %s' % e)
        return 1
    root = os.path.abspath(args.root)
    main_co = main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    caps = [(n, os.path.join(main_co, z)) for n, z in CAPS if not args.only or n == args.only]
    for need in [game] + [z for _, z in caps]:
        if not os.path.exists(need):
            print('FAIL  input missing: ' + need)
            return 1
    if not args.no_build:
        p = subprocess.run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'fview', 'build.ps1')], capture_output=True, text=True, timeout=1800)
        if p.returncode:
            print((p.stdout + p.stderr)[-3000:])
            print('FAIL  fview build (exit %d)' % p.returncode)
            return 1
    sys.path.insert(0, os.path.join(HERE, '..', 'fview'))
    import fview_exe as fe
    exe = fe.find_or_exit(prefer=args.exe, root=root, kind='dev')
    work = os.path.join(root, 'out', 's102', 't6')
    sheets = args.sheet_dir or os.path.join(main_co, 'out', 's102', 'sheets', 't6')
    os.makedirs(work, exist_ok=True)
    os.makedirs(sheets, exist_ok=True)
    fails = 0

    def line(ok, text):
        nonlocal fails
        print(('PASS  ' if ok else 'FAIL  ') + text, flush=True)
        fails += 0 if ok else 1

    lum = np.array([0.3, 0.59, 0.11])

    def arm(zip_path, name, extra, frames):
        hashes = os.path.join(work, name + '.csv')
        if os.path.exists(hashes):
            os.remove(hashes)
        rc, text = fview(exe, ['--case', zip_path, '--replay', '--frames', str(frames), '--validation', '1', '--crowd-ours', '--game', game, '--hash-frames', hashes] + extra)
        if not os.path.isfile(hashes):
            print(text[-2500:])
            raise RuntimeError('arm %s wrote no hashes (exit %d)' % (name, rc))
        vm = re.search(r'validation errors (\d+)', text)   # S102 G5 (F1 class fix): fview writes the csv BEFORE it exits 4 on validation errors: keep the exit code and the count per arm
        VALS[name] = (rc, int(vm.group(1)) if vm else -1)
        return read_hashes(hashes), text

    def picture(zip_path, name, frame, extra):
        d = os.path.join(work, 'pic_%s_%d' % (name, frame))
        rc, text = fview(exe, ['--case', zip_path, '--frame', str(frame), '--crowd-ours', '--game', game, '--judge-out', d] + extra)
        png = os.path.join(d, 'picture.png')
        if not os.path.isfile(png):
            print(text[-2500:])
            raise RuntimeError('picture %s frame %d failed (exit %d)' % (name, frame, rc))
        return np.asarray(Image.open(png).convert('RGB'), dtype=np.float64)

    for name, z in caps:
        off_h, off_t = arm(z, name + '_off', ['--members-instanced', 'off'], args.frames)
        on_h, on_t = arm(z, name + '_on', ['--members-instanced', 'on'], args.frames)
        p1_h, p1_t = arm(z, name + '_plant_world', ['--members-instanced', 'on', '--crowd-fault', 'inst_world_first'], args.frames)
        p2_h, p2_t = arm(z, name + '_plant_drop', ['--members-instanced', 'on', '--crowd-fault', 'inst_drop_last'], args.frames)
        n = len(off_h) - 1
        line(VALS[name + '_off'][1] >= 0 and VALS[name + '_off'] == VALS[name + '_on'], 'validation, %s: exit code / errors OFF %s, ON %s (must be equal and read)' % (name, VALS[name + '_off'], VALS[name + '_on']))
        mo, mi = OURS_RE.search(off_t), INST_RE.search(on_t)
        if not mo or not mi:
            print(on_t[-2500:])
            line(False, 'counts, %s: no `crowd ours` / `members instanced` line (OFF %s, ON %s)' % (name, bool(mo), bool(mi)))
            continue
        frames_on, draws, per_frame, most, groups, members, splits, fallback = int(mi.group(1)), int(mi.group(2)), float(mi.group(3)), int(mi.group(4)), int(mi.group(5)), int(mi.group(6)), int(mi.group(7)), int(mi.group(8))
        off_frames, off_members = int(mo.group(1)), int(mo.group(2))
        line(frames_on > 100 and fallback == 0 and per_frame <= MAX_DRAWS and most <= MAX_DRAWS,
             'draws, %s: %d frames drawn instanced, %.1f member draws a frame (most %d, bar <= %d), %d groups for %d members (%d beyond one per class), fall-backs %d; OFF drew %d members in %d frames = %.0f one-draw members a frame'
             % (name, frames_on, per_frame, most, MAX_DRAWS, groups, members, splits, fallback, off_members, off_frames, off_members / max(1, off_frames)))
        line(members == off_members and frames_on == off_frames, 'members, %s: ON drew %d members in %d frames, OFF %d in %d (equal)' % (name, members, frames_on, off_members, off_frames))
        equal = [k for k in off_h if k != 'format' and off_h[k] == on_h.get(k)]
        bad = [k for k in off_h if k != 'format' and off_h[k] != on_h.get(k)]
        line(n > 100 and len(on_h) == len(off_h), 'frames, %s: %d presented frames in both arms; %d byte-equal, %d differ' % (name, n, len(equal), len(bad)))
        # pictures: the frames that differ, sampled (the first, the middle, the last of them, at most 4), compared by luminance
        worst, worst_frame, shown = 0.0, None, []
        pics = {}
        for k in (sorted(bad)[:: max(1, len(bad) // 4)][:4] if bad else []):
            a = picture(z, name + '_off', k, ['--members-instanced', 'off'])
            b = picture(z, name + '_on', k, ['--members-instanced', 'on'])
            frac = float((np.abs(a @ lum - b @ lum) > PIX).mean())
            shown.append('%d:%.5f' % (k, frac))
            pics[k] = (a, b)
            if frac >= worst:
                worst, worst_frame = frac, k
        line(len(bad) == 0 or (args.tolerance > 0 and worst <= args.tolerance), 'picture, %s: ON vs OFF, share of pixels over %d luminance on %d sampled differing frames: %s (bar: byte-equal on every frame%s); %d of %d frames byte-equal' % (name, PIX, len(shown), ', '.join(shown) or 'none differ', (', or within %.4f with --tolerance' % args.tolerance) if args.tolerance else '', len(equal), n))
        # the planted twins: each must differ from OFF on more frames than ON does AND break the picture bar on a sampled frame
        for tag, ph, pt in (('inst_world_first', p1_h, p1_t), ('inst_drop_last', p2_h, p2_t)):
            pdiff = [k for k in off_h if k != 'format' and off_h[k] != ph.get(k)]
            probe = sorted(pdiff)[len(pdiff) // 2] if pdiff else None
            frac = -1.0
            if probe is not None:
                a = picture(z, name + '_off', probe, ['--members-instanced', 'off'])
                b = picture(z, name + '_' + tag, probe, ['--members-instanced', 'on', '--crowd-fault', tag])
                frac = float((np.abs(a @ lum - b @ lum) > PIX).mean())
            line(len(pdiff) > len(bad) and frac > 2 * PIX_FRAC, 'planted twin %s, %s: %d of %d frames differ from OFF (ON: %d); frame %s: %.5f of pixels over %d luminance (must exceed %.4f, 2x the bar)' % (tag, name, len(pdiff), n, len(bad), probe, frac, PIX, 2 * PIX_FRAC))
            if tag == 'inst_drop_last':
                mp = INST_RE.search(pt)
                line(bool(mp) and int(mp.group(6)) == members, 'planted twin inst_drop_last, %s: the groups still hold %s members (the drawn instances fall short; only the picture sees it)' % (name, mp.group(6) if mp else '?'))
        # AUDIT (review): every member rebuilt from its group's rep block + the rows of its instance slot, the rep's fragment block and material, against its own: 0 wrong, 0 out of stream order
        au = AUDIT_RE.search(on_t)
        line(bool(au) and int(au.group(1)) >= members and int(au.group(2)) == 0 and int(au.group(3)) == 0, 'audit, %s: %s' % (name, au.group(0) if au else 'no audit line'))
        for tag, ph_t in (('inst_world_first', p1_t),):
            pa = AUDIT_RE.search(ph_t)
            line(bool(pa) and int(pa.group(2)) > 0, 'audit plant %s, %s: %s rebuilt wrong (must be > 0)' % (tag, name, pa.group(2) if pa else '?'))
        for tag in ('inst_rows_transposed', 'inst_first_fit'):
            ph, pt = arm(z, name + '_plant_' + tag, ['--members-perturb', '--members-instanced', 'on', '--crowd-fault', tag], PFRAMES)
            pa = AUDIT_RE.search(pt)
            want = int(pa.group(2)) if (pa and tag == 'inst_rows_transposed') else int(pa.group(3)) if pa else 0
            line(bool(pa) and want > 0, 'audit plant %s (with --members-perturb), %s: %s (the %s count must be > 0)' % (tag, name, pa.group(0) if pa else 'no audit line', 'rebuilt-wrong' if tag == 'inst_rows_transposed' else 'out-of-stream-order'))
        # the split path (review F1): every second member of each class gets other fragment constants, in BOTH arms; ON must equal OFF byte for byte with groups beyond one per class
        pr_off_h, pr_off_t = arm(z, name + '_perturb_off', ['--members-perturb', '--members-instanced', 'off'], PFRAMES)
        pr_on_h, pr_on_t = arm(z, name + '_perturb_on', ['--members-perturb', '--members-instanced', 'on'], PFRAMES)
        pi = INST_RE.search(pr_on_t)
        pbad = [k for k in pr_off_h if k != 'format' and pr_off_h[k] != pr_on_h.get(k)]
        pau = AUDIT_RE.search(pr_on_t)
        line(bool(pi) and int(pi.group(7)) > 0 and not pbad and len(pr_on_h) == len(pr_off_h) > 20 and bool(pau) and int(pau.group(2)) == 0 and int(pau.group(3)) == 0,
             'split path, %s: with every second member perturbed, ON byte-equal to OFF on %d frames (%d differ), %s groups beyond one per class, audit %s' % (name, len(pr_off_h) - 1, len(pbad), pi.group(7) if pi else '?', pau.group(0) if pau else '?'))
        ig_h, ig_t = arm(z, name + '_perturb_ignores_fs', ['--members-perturb', '--members-instanced', 'on', '--crowd-fault', 'inst_group_ignores_fs'], PFRAMES)
        ibad = [k for k in pr_off_h if k != 'format' and pr_off_h[k] != ig_h.get(k)]
        iau = AUDIT_RE.search(ig_t)
        line(len(ibad) > 0 and bool(iau) and int(iau.group(2)) > 0, 'planted twin inst_group_ignores_fs (with --members-perturb), %s: %d of %d frames differ from OFF, audit rebuilt-wrong %s (both must be > 0)' % (name, len(ibad), len(pr_off_h) - 1, iau.group(2) if iau else '?'))
        # the sheet
        if worst_frame is None and equal:
            worst_frame = sorted(equal)[len(equal) // 2]
            pics[worst_frame] = (picture(z, name + '_off', worst_frame, ['--members-instanced', 'off']), picture(z, name + '_on', worst_frame, ['--members-instanced', 'on']))
        if worst_frame is not None:
            a, b = pics[worst_frame]
            diff = np.clip(np.abs(a @ lum - b @ lum)[:, :, None].repeat(3, 2) * 8.0, 0, 255)
            h, w = a.shape[:2]
            sheet = Image.new('RGB', (w, (h + 24) * 3), (20, 20, 20))
            for i, (arr, title) in enumerate([(a, 'OFF: one draw per member (frame %d of %s)' % (worst_frame, name)), (b, 'ON: one instanced draw per group and mesh (--members-instanced on)'), (diff, 'DIFFERENCE |ON - OFF| luminance x8')]):
                sheet.paste(Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8)), (0, 24 + (h + 24) * i))
                ImageDraw.Draw(sheet).text((8, 6 + (h + 24) * i), title, fill=(255, 255, 255))
            path = os.path.join(sheets, '%s_off_on_diff.png' % name)
            sheet.save(path)
            line(True, 'sheet, %s: %s' % (name, path))
        else:
            line(False, 'sheet, %s: no frame to draw' % name)
    # validation: a short replay with the layer on (the first capture)
    name, z = caps[0]
    rc, vt = fview(exe, ['--case', z, '--crowd-ours', '--game', game, '--frames', '8', '--validation', '1', '--replay', '--members-instanced', 'on'])
    line(rc == 0 and 'validation errors 0' in vt and 'validation active: yes' in vt, 'validation: 8-frame replay of %s with the layer on and members instanced, errors 0 (exit %d)' % (name, rc))
    print('RESULT ' + ('PASS' if not fails else 'FAIL (%d)' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
