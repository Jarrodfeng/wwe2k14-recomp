#!/usr/bin/env python3
"""S102 lane G1 proof on the Bryan capture (offline, 0 game runs, headless fview): levers 1 and 3 with the settings LOW gives them.

  py -I tools\\g1\\g1_bryan.py [--root DIR] [--sheets DIR] [--capture bryan|match]

Lever 3 (no full-size copies): A = the stream as is; B = --copy-alias 2 --copy-alias-classes 63 (what wwe2k14_rdr_low_no_fullsize_copies sets under LOW).
  Bars: presented-surface hashes of B equal A's on EVERY frame, validation 0, violations 0, elided copies a frame > 0. Planted twin: --copy-alias-fault src_dirty
  must change the pictures (else the comparison is blind). The count of 1280x720 copies the rule does NOT remove is printed: it is the finding.
Lever 1 (small shadow maps): C = --shadows-ours --shadow-map-size 512; D = the same at 256 (what wwe2k14_rdr_low_shadow_small sets). Bars: validation 0 in both;
  the pictures DO differ (else the size never reached the maps); frames are dumped for the sheet (--sheets DIR: C | D | amplified difference, labelled).
Lever 2 (present clear) is not in a headless replay (no swapchain): its proof is tools\\g1\\g1_check.py (unit arithmetic + wiring + twins)."""
import argparse
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
CAPTURES = {
    'bryan': 'out/s101/runs/20261112_122700_s100_owner_bryan_entrance/case_20261112_122719_969_0_pc_run_exit.zip',
    'match': 'out/s101/runs/20261112_131008_s100_owner_match/case_20261112_131027_265_0_pc_run_exit.zip',
}
fails = 0


def line(ok, text):
    global fails
    print(('PASS  ' if ok else 'FAIL  ') + text, flush=True)
    fails += 0 if ok else 1


def main_checkout(here):
    try:
        common = subprocess.run(['git', '-C', here, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return here


def read_hashes(path):
    out = {}
    with open(path) as f:
        for ln in f:
            ln = ln.strip()
            if ln:
                a, b = ln.split(',')
                out[a if a == 'format' else int(a)] = b
    return out


class Arm:
    def __init__(self, exe, zip_path, game, work, name, extra, dump=None, allow_stop=False):
        hashes = os.path.join(work, name + '.csv')
        if os.path.exists(hashes):
            os.remove(hashes)
        cmd = [exe, '--case', zip_path, '--replay', '--frames', '1000', '--validation', '1', '--crowd-ours', '--crowd-cards', '--game', game, '--hash-frames', hashes] + extra
        if dump:
            cmd += ['--dump-frames', ','.join(str(f) for f in dump[0]), '--dump-dir', dump[1]]
        for _ in range(12):
            p = subprocess.run(cmd, capture_output=True, text=True, timeout=1500, errors='replace')
            self.text = (p.stdout or '') + (p.stderr or '')
            self.rc = p.returncode   # S102 G5 (F1 class fix): the exit code is read (4 = validation errors, counted below)
            if 'Not enough graphics memory for the frame ring' not in self.text:
                break
            time.sleep(20)
        self.stopped = not os.path.isfile(hashes)  # a planted fault may stop the replay on a [PROBLEM] before any hash is written: that is a catch
        if self.stopped and not allow_stop:
            print(self.text[-2500:])
            raise RuntimeError('arm %s wrote no hashes' % name)
        self.hashes = {} if self.stopped else read_hashes(hashes)
        if not self.stopped and self.rc not in (0, 4):
            raise RuntimeError('arm %s: fview exit %d (a crash, not a validation count)' % (name, self.rc))
        v = re.findall(r'validation errors (\d+)', self.text)
        self.valid = int(v[-1]) if v else -1
        m = re.search(r'frames planned (\d+) \(skipped (\d+)\), violations (\d+)', self.text)
        self.frames, self.viol = (int(m.group(1)), int(m.group(3))) if m else (0, 0)
        self.elided = sum(int(x) for x in re.findall(r'elided (\d+)', ''.join(l for l in self.text.splitlines(True) if '[copy_alias]   ' in l)))

    def differing(self, other):
        return sum(1 for k in self.hashes if k != 'format' and self.hashes.get(k) != other.hashes.get(k))


def sheet(dirs, frames, labels, out_png, title):
    from PIL import Image, ImageChops, ImageDraw
    rows = []
    frames = [f for f in frames if all(os.path.exists(os.path.join(d, 'frame_%05d.png' % f)) for d in dirs)]
    for f in frames:
        a, b = [Image.open(os.path.join(d, 'frame_%05d.png' % f)).convert('RGB') for d in dirs]
        diff = ImageChops.difference(a, b).point(lambda v: min(255, v * 8))
        rows.append((a, b, diff))
    w, h = rows[0][0].size
    s = 0.5
    tw, th = int(w * s), int(h * s)
    pad = 26
    sheet_img = Image.new('RGB', (tw * 3 + 8, (th + pad) * len(rows) + pad), (24, 24, 24))
    d = ImageDraw.Draw(sheet_img)
    d.text((6, 6), title, fill=(255, 255, 255))
    for i, (a, b, diff) in enumerate(rows):
        y = pad + i * (th + pad)
        for j, (img, lab) in enumerate(zip((a, b, diff), (labels[0], labels[1], 'difference x8'))):
            sheet_img.paste(img.resize((tw, th)), (j * (tw + 4), y + pad - 12))
            d.text((j * (tw + 4) + 4, y), '%s  frame %d' % (lab, frames[i]), fill=(255, 220, 120))
    sheet_img.save(out_png)


def main():
    global fails
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--sheets', default='')
    ap.add_argument('--capture', default='bryan')
    ap.add_argument('--frames', default='40,100,160,220')
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    main_co = main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    z = os.path.join(main_co, CAPTURES[args.capture])
    if not os.path.exists(z):
        print('FAIL  input missing: ' + z)
        return 1
    sys.path.insert(0, os.path.join(root, 'tools', 'fview'))
    import fview_exe as fe
    exe = fe.find_or_exit(root=root, kind='dev')
    work = os.path.join(root, 'out', 'g1_bryan')
    os.makedirs(work, exist_ok=True)
    frames = [int(x) for x in args.frames.split(',')]
    n = args.capture

    # lever 3
    A = Arm(exe, z, game, work, n + '_A', [])
    B = Arm(exe, z, game, work, n + '_B', ['--copy-alias', '2', '--copy-alias-classes', '11'])  # 1|2|8 = bake, shadow, post: low_levers.h kLowCopyClasses (never 16 / 32)
    d = B.differing(A)
    line(d == 0 and len(B.hashes) == len(A.hashes) and A.valid == 0 and B.valid == 0 and B.viol == 0 and B.elided > 0,
         'lever 3 %s: %d frames, %d differ, validation %d/%d, violations %d, copies elided %d (%.1f a frame)' % (n, len(A.hashes) - 1, d, A.valid, B.valid, B.viol, B.elided, B.elided / max(1, B.frames)))
    P = Arm(exe, z, game, work, n + '_P', ['--copy-alias', '2', '--copy-alias-classes', '11', '--copy-alias-fault', 'src_dirty'], allow_stop=True)
    # a stop only counts when the replay says WHY ([rdr][PROBLEM]); a crash or a missing file must not pass as a catch
    stopped_on_problem = P.stopped and '[rdr][PROBLEM]' in P.text
    caught = stopped_on_problem or (not P.stopped and P.differing(A) > 0)
    line(caught, 'lever 3 planted twin (--copy-alias-fault src_dirty) is caught: %s' % ('the replay stopped on a [rdr][PROBLEM]' if stopped_on_problem else '%d frames change (stopped without a problem line: %s)' % (P.differing(A), P.stopped)))

    # the combination players run: shadows ours, 256 maps, half rate (what LOW + levers 1 and 3 make); lever 3 on top must not change a pixel
    E = Arm(exe, z, game, work, n + '_E', ['--shadows-ours', '--shadow-map-size', '256', '--shadow-half'])
    F = Arm(exe, z, game, work, n + '_F', ['--shadows-ours', '--shadow-map-size', '256', '--shadow-half', '--copy-alias', '2', '--copy-alias-classes', '11'])
    d = F.differing(E)
    line(d == 0 and len(F.hashes) == len(E.hashes) and E.valid == 0 and F.valid == 0 and F.viol == 0,
         'LOW combination %s (shadows ours, 256, half rate) with lever 3: %d frames, %d differ, validation %d/%d, violations %d, elided %d' % (n, len(E.hashes) - 1, d, E.valid, F.valid, F.viol, F.elided))

    # lever 1
    sh = args.sheets
    dump = lambda tag: (frames, os.path.join(sh or work, 'dump_' + n + '_' + tag)) if sh or True else None
    for t in ('512', '256'):
        os.makedirs(dump(t)[1], exist_ok=True)
    C = Arm(exe, z, game, work, n + '_C512', ['--shadows-ours', '--shadow-map-size', '512'], dump('512'))
    D = Arm(exe, z, game, work, n + '_D256', ['--shadows-ours', '--shadow-map-size', '256'], dump('256'))
    dd = D.differing(C)
    line(C.valid == 0 and D.valid == 0 and dd > 0, 'lever 1 %s: validation %d/%d; 256-texel maps change %d of %d frames (>0: the size reaches the maps)' % (n, C.valid, D.valid, dd, len(C.hashes) - 1))
    if sh:
        os.makedirs(sh, exist_ok=True)
        try:
            sheet([dump('512')[1], dump('256')[1]], frames, ['maps 512 (default)', 'maps 256 (LOW lever)'], os.path.join(sh, 'g1_shadow_small_%s.png' % n), 'G1 lever 1: %s capture, shadows ours, shadow maps 512 vs 256' % n)
            print('sheet: ' + os.path.join(sh, 'g1_shadow_small_%s.png' % n))
        except Exception as e:
            line(False, 'sheet failed: %r' % (e,))
    print('G1 BRYAN: %s' % ('all passed' if not fails else '%d FAILED' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
