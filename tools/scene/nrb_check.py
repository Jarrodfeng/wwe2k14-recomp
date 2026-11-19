#!/usr/bin/env python3
"""S103 lane NRB / NRBF: the own-skeleton pieces the OWNER's body model draws (docs\\research\\s103\\NRB_pieces.md), proven offline on the rig. 0 game runs.

  py -I tools\\scene\\nrb_check.py [--root DIR]

Record: run 29's frame_000622.rsc (out\\s103\\run29_case.zip, guest 15182: Lesnar ch343 slot 0, Bryan ch324 slot 1, the referee ch255 slot 6, 7 own-skeleton pieces), drawn by
out\\tools\\frame_rig.exe --body-scale-one (the case has no body-scale table). The 7 links are the stream join's (wrestler_ours_check --wants: each object's geometry = that file entry).
The game's SUBM update copies the owner skeleton's rows into each piece through a name-built node map, so the merged body model already draws the pieces: no model of its own, no second body.
Arms (each must hold, else FAIL):
  base     no links: the 7 pieces are holes (H0 holes, drawn 5)
  linked   7 --piece-link: wrestler holes H0 - 7 = 0, drawn 5, no hole names a piece, the 7 are counted "drawn by their owner's body"
  drop     6 links (the referee's 21011 left out): exactly 1 hole, and it names skel:A95B8B00
  hide     plant (--piece-link-plant-hide): links told but the game did not draw the pieces this frame (no light ref): they are holes again (review F5, fail closed)
  same     the linked picture equals the base picture (0 px): nothing is drawn twice or moved
  kind4    (S103 PZF2, EB) Bryan's asset byte-patched model_key:32402 -> 32404 (his entrance PACH, which merges its own pieces): base 7 holes, 7 links -> 0 holes, 0 px
  nomesh   (S103 PZF2, EB) a link naming a piece id the owner's resident model has no mesh of (12345): exactly 1 hole, naming the piece (the draw-time check, kind-independent)
(The owner-identity refusal, review F1, is the contributor's: wrestler_emit selftest "NRBF plant: owner skeleton != the drawn slot body".)
A missing input FAILS (never skips).
"""
import argparse
import os
import re
import subprocess
import sys
import zipfile

LINKS = ['skel:A8FB30B0=1:10000', 'skel:A8FB8810=1:20002', 'skel:A910B850=0:10000', 'skel:A910E270=0:20002',
         'skel:A95B04A0=6:10000', 'skel:A95B2D10=6:20002', 'skel:A95B8B00=6:21011']
PIECES = [l.split('=')[0] for l in LINKS]


def main_checkout(here):
    try:
        common = subprocess.run(['git', '-C', here, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return here


def run_rig(exe, rec, game, prefix, extra):
    p = subprocess.run([exe, '--record', rec, '--game', game, '--prefix', prefix, '--body-scale-one'] + extra, capture_output=True, text=True, timeout=600)
    out = p.stdout + p.stderr
    m = re.search(r'family wrestlers objects (\d+) drawn (\d+) skipped (\d+) holes (\d+)(?: why (.*))?', out)
    if p.returncode != 0 or not m:
        print('FAIL rig exit %d, no wrestlers line\n%s' % (p.returncode, out[-2000:]))
        sys.exit(1)
    by_owner = re.search(r"pieces drawn by their owner's body (\d+)", out)
    return int(m.group(2)), int(m.group(4)), (m.group(5) or ''), prefix + '_whole.png', int(by_owner.group(1)) if by_owner else -1


def changed_px(a, b):   # pixels whose largest channel difference is over 12 (the rig's own run-to-run noise is far below)
    from PIL import Image, ImageChops
    d = ImageChops.difference(Image.open(a).convert('RGB'), Image.open(b).convert('RGB')).convert('L').point(lambda v: 255 if v > 12 else 0)
    return sum(1 for v in d.tobytes() if v)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')))
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    main_root = main_checkout(root)
    exe = os.path.join(root, 'out', 'tools', 'frame_rig.exe')
    zp = os.path.join(main_root, 'out', 's103', 'run29_case.zip')
    game = os.path.join(main_root, 'game_data', 'extracted')
    for need in (exe, zp, game):
        if not os.path.exists(need):
            print('FAIL missing input: ' + need)
            return 1
    work = os.path.join(root, 'out', 's103', 'NRB')
    os.makedirs(work, exist_ok=True)
    rec = os.path.join(work, 'r29_frame_000622.rsc')
    with zipfile.ZipFile(zp) as z, open(rec, 'wb') as f:
        f.write(z.read('capture/scene/frame_000622.rsc'))
    ok = True

    def chk(c, what):
        nonlocal ok
        print(('ok    ' if c else 'FAIL  ') + what)
        ok = ok and c

    links = [x for l in LINKS for x in ('--piece-link', l)]
    d0, h0, w0, p0, o0 = run_rig(exe, rec, game, os.path.join(work, 'base'), [])
    d1, h1, w1, p1, o1 = run_rig(exe, rec, game, os.path.join(work, 'linked'), links)
    d2, h2, w2, p2, o2 = run_rig(exe, rec, game, os.path.join(work, 'drop'), links[:-2])
    d3, h3, w3, p3, o3 = run_rig(exe, rec, game, os.path.join(work, 'hide'), links + ['--piece-link-plant-hide'])
    print('base drawn %d holes %d (%s) | linked drawn %d holes %d by-owner %d | drop drawn %d holes %d (%s) | hide drawn %d holes %d by-owner %d' % (d0, h0, w0, d1, h1, o1, d2, h2, w2, d3, h3, o3))
    chk(d0 == 5 and h0 == 7 and o0 == 0, 'base: 5 bodies drawn, exactly the 7 pieces are holes (NRA removed the apron skirts)')
    chk(d1 == 5 and h1 == 0 and o1 == 7 and not any(p in w1 for p in PIECES), 'linked: 5 bodies drawn, wrestler holes 0, the 7 pieces counted as drawn by their owner')
    chk(h2 == 1 and o2 == 6 and 'skel:A95B8B00' in w2, 'drop one link: exactly 1 hole, named skel:A95B8B00')
    chk(h3 == 7 and o3 == 0 and any(p in w3 for p in PIECES), 'plant hide (no light ref = the game hid the pieces): all 7 are holes again, named')
    px_same = changed_px(p0, p1)
    print('pixels: base vs linked %d' % px_same)
    chk(px_same == 0, 'linked picture == base picture (0 px: nothing is drawn twice or moved)')
    # S103 PZF2 (EB, docs\research\s103\EB_entrance_body.md section 3): Bryan's entrance body is its own model PACH (key 32404) that merges its own pieces 10000 / 20002. The arm byte-patches the record's
    # Bryan asset (model_key:32402 -> model_key:32404: the same length, the strings table is not moved) and links the 7 pieces: 0 holes, 0 px against the patched base.
    raw = open(rec, 'rb').read()
    n_key = raw.count(b'model_key:32402')
    chk(n_key >= 1, 'record carries Bryan\'s match key model_key:32402 (%d)' % n_key)
    rec4 = os.path.join(work, 'r29_frame_000622_k4.rsc')
    open(rec4, 'wb').write(raw.replace(b'model_key:32402', b'model_key:32404'))
    d4, h4, w4, p4, o4 = run_rig(exe, rec4, game, os.path.join(work, 'k4_base'), [])
    d5, h5, w5, p5, o5 = run_rig(exe, rec4, game, os.path.join(work, 'k4_linked'), links)
    print('kind-4 base drawn %d holes %d | kind-4 linked drawn %d holes %d by-owner %d' % (d4, h4, d5, h5, o5))
    chk(d4 == 5 and h4 == 7, 'kind 4 base: Bryan\'s entrance model drawn, the 7 pieces are holes')
    chk(d5 == 5 and h5 == 0 and o5 == 7 and not any(p in w5 for p in PIECES), 'kind 4 + the 7 links: wrestler holes 0 (the entrance PACH carries its own 10000 / 20002 meshes)')
    px4 = changed_px(p4, p5)
    print('pixels: kind-4 base vs linked %d' % px4)
    chk(px4 == 0, 'kind 4 linked picture == kind 4 base picture (0 px)')
    # S103 PZF2 (EB's draw-time check, kind-independent): a piece the owner's resident model has NO mesh of is a hole, named, even when every other proof passed. 12345 is no piece of Bryan's PACH.
    bad = links[:]
    bad[bad.index('skel:A8FB30B0=1:10000')] = 'skel:A8FB30B0=1:12345'
    d6, h6, w6, p6, o6 = run_rig(exe, rec, game, os.path.join(work, 'nomesh'), bad)
    print('mesh twin drawn %d holes %d by-owner %d (%s)' % (d6, h6, o6, w6))
    chk(d6 == 5 and h6 == 1 and o6 == 6 and 'skel:A8FB30B0' in w6 and 'no mesh of piece 12345' in w6, 'mesh-check twin: a linked piece the owner model has no mesh of is exactly 1 hole, naming the piece')
    print('nrb_check: ' + ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
