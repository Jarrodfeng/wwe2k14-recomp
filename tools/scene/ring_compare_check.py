#!/usr/bin/env python3
"""S103 lanes C3 + C7 check: our recomputed ring rows against the GAME's own RingRows in a real mode-1 record (--wwe2k14_rdr_ring_d3=1, scene records on).
Our rows of record j (stamp j) must equal the game's RingRows of record j + 1 (stamped j); the planted twin (the game's rows three records later) must NOT pass.
Reads the case zip READ ONLY (it sits in out\\: never moved or deleted). 0 game runs.
  py -I tools\\scene\\ring_compare_check.py [--root DIR] [--zip PATH] [--every N] [--classes rope,turnbuckle,cover,post,skirt]
Default = the C3 lane's call (ropes only, run 16's zip, bit-equal). Per class bound (TOL, S103 C7): a blanket pass is never given - each non-zero bound names its cause, and a class passes only with
differ 0, no_ours 0 (every game entry has a row of ours) and a worst difference within the bound; the twin must have entries OUTSIDE the bound (else the class cannot tell frames apart: allowed only for
static pieces, and printed)."""
import argparse
import hashlib
import os
import re
import subprocess
import sys
import zipfile

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
MAIN = r'C:\Users\Jarrod\wwe2k14-recomp'
ZIP16 = MAIN + r'\out\build\win-amd64-release\out\cases\case_20261116_023239_766_0_pc_run_exit.zip'

# class -> (bound, twin_needed, reason). The bound is the largest absolute float difference over the game's palette nodes.
TOL = {
    'rope': (0.0, True, 'bit-equal (run 27: 336 of 336)'),
    'skirt': (0.0, True, 'bit-equal (run 27: 84 of 84)'),
    'turnbuckle': (1.4e-4, True, 'S103 C3 tb_fit: the rotation is rebuilt with libm sin / cos, the game uses the polynomial sub_825D0618 (vmsum4fp128, [C] listing; its error is not reproduced: unconfirmed as the whole cause), about 4e-7 relative x the 285..332 radius; run 27 worst 9.2e-5'),
    'cover': (2.0e-4, True, 'same cause as the turnbuckle (corner 0 = angle 0 is bit-equal; every other corner is off, run 27 worst 1.5e-4 = the fit of STAGE_LIVE_WRITERS 3c); bound = 1.5e-4 rounded up'),
    'post': (1.0e-6, False, 'posts never move, so the twin (a later record) is identical to the real pairing: it cannot tell frames apart; run 27 worst 3.0e-7 (float rounding of the skin product)'),
}
RX = re.compile(r'SUMMARY records with both (\d+), game rope entries (\d+): exact (\d+), near (\d+), differ (\d+), no_ours (\d+), worst (\S+)')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--zip', default=ZIP16)
    ap.add_argument('--every', type=int, default=10)
    ap.add_argument('--classes', default='rope')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    zp = a.zip
    if not os.path.isfile(zp) and zp.lower().startswith(MAIN.lower()):
        zp = os.path.join(root, zp[len(MAIN) + 1:])
    if not os.path.isfile(zp) and not os.path.isabs(zp) and os.path.isfile(os.path.join(MAIN, zp)):
        zp = os.path.join(MAIN, zp)   # a worktree has no out\ of its own: the main checkout's
    if not os.path.isfile(zp):
        print('FAIL: the case zip is missing: ' + a.zip)
        return 1
    # S103 C7: the extracted frames are cached PER ZIP (key = path + size + mtime). They were cached by frame number alone, so a second zip with the same frame numbers silently paired
    # records of two different runs (the run 27 check: 82 % "no_ours" from run 16's stale files).
    st = os.stat(zp)
    key = hashlib.sha1(('%s|%d|%d' % (os.path.abspath(zp).lower(), st.st_size, int(st.st_mtime))).encode()).hexdigest()[:12]
    base = os.path.join(root, 'out', 's103', 'c7', 'gate')
    out = os.path.join(base, key)
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(base, 'ring_compare.exe')
    b = subprocess.run([CLANG, '-std=c++20', '-O1', '-D_CRT_SECURE_NO_WARNINGS', '-fuse-ld=lld', os.path.join(root, 'tools', 'scene', 'ring_compare.cpp'),
                        os.path.join(root, 'src', 'renderer_scene', 'record_io.cpp'), os.path.join(root, 'src', 'renderer_scene', 'live_scene.cpp'), '-o', exe], capture_output=True, text=True)
    if b.returncode:
        print('FAIL: did not compile\n' + b.stdout + b.stderr)
        return 1
    st_run = subprocess.run([exe, '--selftest'], capture_output=True, text=True)   # S103 RLB: ours-LO ropes are wanted; the mixed hi / lo game frame pairs by (r5, rank), not by call
    print(st_run.stdout.strip())
    if st_run.returncode:
        print('FAIL: ring_compare --selftest (the rope LOD pairing)')
        return 1
    z = zipfile.ZipFile(zp)
    frames = sorted(int(m.group(1)) for m in (re.search(r'frame_(\d+)\.rsc$', n) for n in z.namelist()) if m)
    have = set(frames)
    picks = [f for f in frames if f % a.every == 0 and f + 1 in have and f + 3 in have]
    if len(picks) < 10:
        print('FAIL: too few record pairs (%d)' % len(picks))
        return 1

    def extract(f):
        p = os.path.join(out, 'frame_%06d.rsc' % f)
        if not os.path.isfile(p):
            with open(p, 'wb') as fh:
                fh.write(z.read('capture/scene/frame_%06d.rsc' % f))
        return p

    def run(cls, f, g, tol):
        env = dict(os.environ, RC_GAME=extract(g), RC_CLASS=cls, RC_TOL=repr(tol))
        r = subprocess.run([exe, extract(f)], capture_output=True, text=True, env=env)
        m = RX.search(r.stdout)
        if not m:
            return None
        return [int(m.group(i)) for i in range(2, 7)] + [float(m.group(7))]   # entries exact near differ no_ours worst

    bad = 0
    for cls in [c.strip() for c in a.classes.split(',') if c.strip()]:
        if cls not in TOL:
            print('FAIL: unknown class ' + cls)
            return 1
        tol, twin_needed, why = TOL[cls]
        tot = [0, 0, 0, 0, 0, 0.0]
        twin = [0, 0, 0, 0, 0, 0.0]
        for f in picks:
            for dst, g in ((tot, f + 1), (twin, f + 3)):
                r = run(cls, f, g, tol)
                if r is None:
                    print('FAIL: no summary for frame %d class %s' % (f, cls))
                    return 1
                for i in range(5):
                    dst[i] += r[i]
                dst[5] = max(dst[5], r[5])
        ent, ex, nr, df, no, worst = tot
        print('%s: pairs %d, game entries %d, exact %d, near %d (bound %g), differ %d, no_ours %d, worst %.3g' % (cls, len(picks), ent, ex, nr, tol, df, no, worst))
        print('  bound: ' + why)
        if ent == 0 or no or df or ex + nr != ent or worst > tol:
            print('FAIL: %s: our rows are not within the bound of the game\'s RingRows of the next record' % cls)
            bad += 1
        # the twin: the game's rows three records later. At least one entry must be OUTSIDE the bound, else the comparison cannot tell frames apart.
        print('  twin (game rows 3 records later): outside the bound %d of %d, worst %.3g' % (twin[3], twin[0], twin[5]))
        if twin[3] == 0:
            if twin_needed:
                print('FAIL: %s: the planted twin is not caught (the comparison cannot tell frames apart)' % cls)
                bad += 1
            else:
                print('  (untested by twin, by design: static piece)')
    print('RESULT', 'OK' if bad == 0 else 'FAIL')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
