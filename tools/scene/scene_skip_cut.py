#!/usr/bin/env python3
r"""S103 lane H: the emulated cut of the scene skips (docs\renderer\SCENE_SKIPS.md section 8; the model is tools\crowd\bake_skip_cut.py, OWN_THE_PASS.md 9 item 1). 0 game runs.

Takes a capture, removes every draw the scene skips would leave out (the 3D scene classes: ModelPart, Crowd, Particle, Shadow, Post, the three 'Other' submitters of SCENE_SKIPS.md 5,
and the unnamed draws that run a scene effect), and checks:
  1. DECODE   fview replays the cut capture (--draw-drop, every class, repeatable): 0 validation errors, bound draws == total - dropped (the cut removed exactly what it counted).
  2. REMNANT  after the cut no draw of a scene class and no unnamed draw of a scene effect is left (the double check's oracle: stream draws of a stamped frame that must be 0).
  3. LEFTOVER (the state-leak census): for each remaining (remnant) draw, which images it samples: written LAST by a scene op of the same frame = a leftover read (the remnant would read
     a picture nobody draws any more), written by a remnant op = fine, written by no op of this frame = persistent (its writer was an earlier frame; reported by size so landing 2 knows what to own).
     The remnant's own targets last written by scene ops are counted apart ('over_scene': the remnant is bound over our post output: FRAME_CONTRACT 6).
  py -I tools\scene\scene_skip_cut.py [--capture DIR] [--frames N] [--json OUT] [--plant NAME]
Plants (tests only; each must exit 1): drop_nothing (no class cut), keep_unnamed (the unnamed scene draws stay: the 192 a frame in brock_losswin), keep_post (the post class stays).
Exit 0 = all three hold (leftover reads are REPORTED, not failed: a non-zero count is what landing 2 must own); 1 = a check fails or an input cannot be read. Scratch output goes under out\."""
import argparse
import collections
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
MAIN = r'C:\Users\Jarrod\wwe2k14-recomp'
sys.path.insert(0, os.path.join(ROOT, 'tools', 'crowd'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'particles'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'solver'))
import numpy as np  # noqa: E402

# names.h DrawClass
UNNAMED, MODELPART, CROWD, SPRITE, TEXT, MOVIE, PARTICLE, SHADOW, POST, CLEAR, OTHER = range(11)
SCENE_CLASSES = {'model_part': MODELPART, 'crowd': CROWD, 'particle': PARTICLE, 'shadow': SHADOW, 'post': POST, 'other': OTHER}
# the 'Other' submitters that are scene work (SCENE_SKIPS.md 5): [start, end) of the origin table rows
SCENE_OTHER = [(0x82799B60, 0x8279A328), (0x8279A328, 0x8279ABE8), (0x82C20AB8, 0x82C20BC8)]


def main_path(p):
    return p if os.path.isabs(p) else os.path.join(MAIN, p)


def fview_run(capture, frames, drops):
    exe = os.path.join(MAIN, 'out', 'renderer', 'dev', 'fview.exe')
    if not os.path.isfile(exe):
        raise SystemExit('FAIL: %s is missing (pwsh tools\\renderer\\build.ps1)' % exe)
    cmd = [exe, '--capture', capture, '--validation', '1', '--frames', str(frames)]
    for d in drops:
        cmd += ['--draw-drop', d]
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=MAIN, timeout=1800)
    out = r.stdout + r.stderr
    m = re.search(r'capture: (\d+) frames, (\d+) draws, (\d+) bound', out)
    e = re.search(r'validation errors (\d+)', out)
    if not m or not e:
        raise SystemExit('FAIL: fview gave no summary (exit %d):\n%s' % (r.returncode, out[-1500:]))
    dropped = {}
    for d in re.finditer(r"draw-drop '(@\S+)': last frame: (\d+) draws dropped \((\d+) over every bind\)", out):
        dropped[d.group(1)] = int(d.group(3))
    return dict(frames=int(m.group(1)), draws=int(m.group(2)), bound=int(m.group(3)), errors=int(e.group(1)), dropped=dropped)


# The 15 hooked functions (tools\renderer\scene_skip_unit.py HOOKS) and the attachments the call graph cannot show: a method reached through a vtable from a hooked node.
HOOKED = [0x82C04DF0, 0x82C13A80, 0x82519508, 0x827FD698, 0x825D0438, 0x825D01F0, 0x827E9740, 0x827EA7B8, 0x827EF070, 0x827EBE78, 0x8279ACD8, 0x82254E30, 0x82C1E840, 0x8276EAA0, 0x82C1F728]
# Scene draws whose submitter reaches no hook, KNOWN and reported as OPEN (SCENE_SKIPS.md section 10): not a failure of this check, a named piece of work. Anything else unreached fails.
KNOWN_OPEN = {}   # S103 T: 0x82815840 attaches to the plane node (ATTACH), 0x82C1F728 is hooked
ATTACH = {   # method -> the hooked node that calls it through a vtable (each from a lodged page)
    0x82C1E4C0: 0x82C1E840, 0x82C28A70: 0x82C1E840, 0x82C28F68: 0x82C1E840, 0x82C29478: 0x82C1E840, 0x82C29818: 0x82C1E840, 0x82C2A868: 0x82C1E840, 0x82C2AF80: 0x82C1E840,   # OWN_THE_PASS.md 11: the effects' slot 2
    0x82C2A540: 0x82C1E840, 0x82C2BBE8: 0x82C1E840, 0x82C2BF00: 0x82C1E840,
    0x82A0D0E0: 0x82254E30, 0x82A0DAC8: 0x82254E30, 0x82A0E3D8: 0x82254E30,   # OWN_PARTICLES_NODE.md 1: slot-3 methods of the entries sub_82A0AED8 draws
    0x82815840: 0x827EF070,   # S103 T (SCENE_SKIPS.md 4a): run-5 trace, 120 of 120 calls from 0x827EEEB4 = sub_827EED58 (the plane walker) <- sub_827EEF80 <- the hooked node sub_827EF070
    0x827E95D8: 0x827E9740, 0x827EA098: 0x827EA7B8,                           # the nodes' frame functions (direct calls, listed for the vtable-less start)
}


def reach_report(sites, site_class, unnamed_pairs, pairs_tagged):
    import sqlite3
    db = os.path.join(MAIN, 'out', 'harness', 'index', 'index.db')
    con = sqlite3.connect('file:%s?mode=ro' % db, uri=True)
    hooked = set(HOOKED)

    def containing(a):
        r = con.execute('SELECT addr, size FROM funcs WHERE addr <= ? AND has_body = 1 ORDER BY addr DESC LIMIT 1', (a,)).fetchone()
        return r[0] if r and a < r[0] + r[1] else None

    memo = {}

    def reaches(fn):
        # BFS up the callers; true when a hooked function (or an attached method) is on a path
        if fn in memo:
            return memo[fn]
        seen, todo, hit = {fn}, [fn], None
        while todo and hit is None and len(seen) < 4000:
            cur = todo.pop()
            if cur in hooked:
                hit = cur
                break
            if cur in ATTACH:
                hit = ATTACH[cur]
                break
            for (src,) in con.execute('SELECT DISTINCT src FROM edges WHERE dst = ?', (cur,)):
                if src not in seen:
                    seen.add(src)
                    todo.append(src)
        memo[fn] = hit
        return hit

    unreached, draws, reached, open_sites = [], 0, 0, []
    for site, n in sites.items():
        fn = containing(site)
        draws += n
        hit = reaches(fn) if fn else None
        if hit:
            reached += 1
        elif fn in KNOWN_OPEN:
            open_sites.append('%s: %d draws' % (KNOWN_OPEN[fn], n))
        else:
            unreached.append('0x%08X in %s (class %d, %d draws)' % (site, ('sub_%08X' % fn) if fn else '?', site_class[site], n))
    un_draws = sum(unnamed_pairs.values())
    shared = sum(n for (vs, ps, _), n in unnamed_pairs.items() if (vs, ps) in pairs_tagged)
    return dict(sites=len(sites), draws=draws, reached=reached, unreached_sites=unreached, open_sites=open_sites, unnamed_draws=un_draws, unnamed_programs=len(unnamed_pairs), unnamed_shared=shared)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--capture', default=os.path.join('out', 'captures', 'brock_losswin'))
    ap.add_argument('--frames', type=int, default=220)
    ap.add_argument('--json', default='')
    ap.add_argument('--plant', choices=['drop_nothing', 'keep_unnamed', 'keep_post'])
    ap.add_argument('--skip-fview', action='store_true', help='census only (no replay)')
    ap.add_argument('--selftest', action='store_true', help='the real cut (must pass), then each planted twin (each must exit 1)')
    a = ap.parse_args()
    if a.selftest:
        base = [sys.executable, '-I', os.path.abspath(__file__), '--capture', a.capture, '--frames', str(a.frames)]
        real = subprocess.run(base + (['--json', a.json] if a.json else []), capture_output=True, text=True)
        print(real.stdout.strip())
        if real.returncode:
            print('FAIL: the real cut failed:', real.stderr[-800:])
            return 1
        for plant in ('drop_nothing', 'keep_unnamed', 'keep_post'):
            p = subprocess.run(base + ['--plant', plant, '--skip-fview'], capture_output=True, text=True)
            if p.returncode == 0:
                print('FAIL: planted twin %s was not caught' % plant)
                return 1
            print('twin %s: caught' % plant)
        return 0
    cap_path = main_path(a.capture)

    import cards_census as cc  # noqa: E402  (the capture reader; reads relative paths from the main checkout)
    cap = cc.Cap(cap_path)
    nframes = min(a.frames, len(cap.frames))
    classes = dict(SCENE_CLASSES)
    if a.plant == 'keep_post':
        classes.pop('post')
    if a.plant == 'drop_nothing':
        classes = {}

    # ---- the remnant's effects: whatever the capture's own UI classes run
    ui_effects = set()
    for fi in range(0, nframes, max(1, nframes // 20)):
        f = cap.frame(fi)
        if not f.has_origins or not len(f.draws):
            continue
        veff = cap.effs(f.draws['vs_hash'])
        k = f.origins['klass']
        for c in (SPRITE, TEXT, MOVIE, CLEAR):
            ui_effects.update(str(x).split('/')[0] for x in veff[k == c])

    def is_scene(klass, site, effect, cutset=None):
        # cutset None = the TRUTH (every scene class, every scene submitter, every unnamed draw that runs a scene effect); else the planted / real cut set
        cl = SCENE_CLASSES if cutset is None else classes
        if klass in cl.values() and klass != OTHER:
            return True
        if klass == OTHER and 'other' in cl:
            return any(lo <= site < hi for lo, hi in SCENE_OTHER)
        if klass == UNNAMED and (cutset is None or a.plant not in ('keep_unnamed', 'drop_nothing')):
            return effect.split('/')[0] not in ui_effects
        return False

    tot = collections.Counter()
    leftover = collections.Counter()      # (w, h, format, effect) -> reads of images last written by a scene op
    persistent = collections.Counter()    # (w, h, format) -> reads of images no op of the frame wrote
    over_scene = collections.Counter()    # remnant passes whose first target was last written by a scene op
    remnant_effects = collections.Counter()
    remnant_targets = collections.Counter()   # first colour target image of the pass each remnant draw sits in (the double check by target: one image, the final colour)
    remaining_scene = 0
    sites = collections.Counter()          # F5: (call_site) of every tagged scene draw (by class), with its class name
    site_class = {}
    pairs_tagged = set()                   # (vs_hash, ps_hash) of tagged scene draws
    unnamed_pairs = collections.Counter()  # (vs_hash, ps_hash, effect) of unnamed scene draws
    for fi in range(nframes):
        f = cap.frame(fi)
        D = f.draws
        if not len(D) or not f.has_origins:
            tot['frames_no_origins'] += 1
            continue
        veff = cap.effs(D['vs_hash'])
        klass, site = f.origins['klass'].astype(int), f.origins['call_site'].astype(np.int64)
        scene = np.array([is_scene(int(klass[i]), int(site[i]), str(veff[i]), True) for i in range(len(D))], bool)
        truth = np.array([is_scene(int(klass[i]), int(site[i]), str(veff[i])) for i in range(len(D))], bool)
        for i in np.nonzero(truth)[0]:
            if int(klass[i]) == UNNAMED:
                unnamed_pairs[(int(D['vs_hash'][i]), int(D['ps_hash'][i]), str(veff[i]))] += 1
            else:
                sites[int(site[i])] += 1
                site_class[int(site[i])] = int(klass[i])
                pairs_tagged.add((int(D['vs_hash'][i]), int(D['ps_hash'][i])))
        copies = cc.table(f, cc.SEC_COPIES, cc.COPY_DT)
        tot['frames'] += 1
        tot['draws'] += len(D)
        tot['scene_draws'] += int(scene.sum())
        remaining_scene += int((truth & ~scene).sum())   # the oracle: a scene draw (by the full rules) the cut leaves in the stream
        # passes in op order: class = scene when every draw of the pass is a scene draw, remnant when none is, 'mixed' otherwise
        passes, cur = [], None
        for pos, (kind, idx) in enumerate(f.ops):
            kind, idx = int(kind), int(idx)
            if kind == 0:
                t = f.targets[idx]
                imgs = [int(t['color'][j]['image']) for j in range(int(t['count']))] + ([int(t['depth']['image'])] if int(t['depth']['image']) else [])
                cur = dict(imgs=imgs, draws=[], copies=[])
                passes.append(cur)
            elif cur is not None and kind == 2:
                cur['draws'].append(idx)
            elif cur is not None and kind == 3:
                cur['copies'].append(idx)
        writer = {}      # image -> 'scene' | 'remnant'
        sl = f.slots
        tf, tc = D['tex_first'].astype(np.int64), D['tex_count'].astype(np.int64)
        for p in passes:
            ds = p['draws']
            n_scene = int(sum(scene[d] for d in ds))
            cls = 'scene' if ds and n_scene == len(ds) else 'remnant' if ds and n_scene == 0 else 'mixed' if ds else 'empty'
            tot['passes_' + cls] += 1
            if cls in ('remnant', 'mixed') and p['imgs'] and writer.get(p['imgs'][0]) == 'scene':
                im = cap.imgs.get(p['imgs'][0])
                over_scene[(int(im['w']), int(im['h'])) if im is not None else None] += 1
            for d in ds:
                if scene[d]:
                    continue
                remnant_effects[str(veff[d])] += 1
                remnant_targets[p['imgs'][0] if p['imgs'] else 0] += 1
                for r in sl[int(tf[d]):int(tf[d]) + int(tc[d])]:
                    img = int(r['image'])
                    w = writer.get(img)
                    im = cap.imgs.get(img)
                    key = (int(im['w']), int(im['h']), int(im['format'])) if im is not None else (0, 0, 0)
                    if w == 'scene':
                        leftover[key + (str(veff[d]),)] += 1
                        tot['leftover_reads'] += 1
                    elif w is None:
                        persistent[key] += 1
                        tot['persistent_reads'] += 1
                    else:
                        tot['remnant_reads'] += 1
            if cls != 'empty':
                for img in p['imgs']:
                    writer[img] = 'scene' if cls == 'scene' else ('remnant' if cls == 'remnant' else writer.get(img, 'remnant'))
                for c in p['copies']:
                    writer[int(copies[c]['d']['image'])] = 'scene' if cls == 'scene' else 'remnant'

    res = dict(capture=a.capture, frames=nframes, census=dict(tot), remaining_scene_draws_after_cut=remaining_scene,
               leftover_reads_by_image=[dict(w=k[0], h=k[1], fmt=k[2], effect=k[3], n=n) for k, n in leftover.most_common(20)],
               persistent_reads_by_image=[dict(w=k[0], h=k[1], fmt=k[2], n=n) for k, n in persistent.most_common(12)],
               remnant_passes_over_scene_surface=[dict(size=k, n=n) for k, n in over_scene.most_common(8)],
               remnant_effects=dict(remnant_effects.most_common(12)), remnant_draws_by_target_image=dict(remnant_targets.most_common(8)))
    # ---- F5: does every scene draw's call site reach a HOOKED function (13 hooks) through the caller closure (q's index, offline)?
    reach = reach_report(sites, site_class, unnamed_pairs, pairs_tagged)
    res['reach'] = reach
    print(('ok   ' if not reach['unreached_sites'] else 'FAIL ') + 'REACH: %d scene call sites (%d draws): %d reach a hooked function directly or by the named vtable attachments, %d reach none %s'
          % (reach['sites'], reach['draws'], reach['reached'], len(reach['unreached_sites']), reach['unreached_sites'][:6]))
    print('     unnamed scene draws: %d (%d distinct programs); %d share their vs/ps pair with a tagged scene draw that reaches a hook (REMNANT_SEAM H7: skipped by walk, not by tag)'
          % (reach['unnamed_draws'], reach['unnamed_programs'], reach['unnamed_shared']))
    for o in reach['open_sites']:
        print('OPEN  ' + o)
    bad_reach = 1 if reach['unreached_sites'] else 0
    # ---- 1. DECODE through fview with the same cut
    bad = bad_reach
    if not a.skip_fview:
        drops = ['@class=%s' % c for c in classes] + (['@class=unnamed'] if a.plant not in ('keep_unnamed', 'drop_nothing') else [])
        fv = fview_run(cap_path, nframes, drops)
        res['fview'] = fv
        sum_dropped = sum(fv['dropped'].values())
        ok_decode = fv['errors'] == 0 and fv['bound'] + sum_dropped == fv['draws']
        print(('ok   ' if ok_decode else 'FAIL ') + 'DECODE: %d frames, %d draws, %d dropped, %d bound, validation errors %d' % (fv['frames'], fv['draws'], sum_dropped, fv['bound'], fv['errors']))
        bad += 0 if ok_decode else 1
        # the fview cut and this tool's scene set count the same unnamed draws? (informational: the fview drop takes every unnamed draw, this tool only the scene effects)
        res['unnamed_dropped_by_fview'] = fv['dropped'].get('@class=unnamed', 0)
    # ---- 2. REMNANT oracle: nothing of a scene class is left
    ok_rem = remaining_scene == 0
    scene_left = tot['draws'] - tot['scene_draws']
    print(('ok   ' if ok_rem else 'FAIL ') + 'REMNANT: %d draws would be left; scene-class draws left after the cut %d' % (scene_left, remaining_scene))
    bad += 0 if ok_rem else 1
    # the remnant must not run a scene effect (a missed unnamed scene draw)
    scene_fx = [e for e in remnant_effects if str(e).split('/')[0] not in ui_effects]   # (a plant that keeps scene draws puts their effects here)
    ok_fx = not scene_fx
    print(('ok   ' if ok_fx else 'FAIL ') + 'REMNANT EFFECTS: %s' % (', '.join('%s x%d' % (e, remnant_effects[e]) for e in list(remnant_effects)[:6]) or 'none'))
    if scene_fx:
        print('     scene effects left in the remnant: ' + ', '.join(sorted(set(scene_fx))[:8]))
    bad += 0 if ok_fx else 1
    print('TARGETS: remnant draws by first colour target image %s (the double check by target expects ONE: the final colour image)' % dict(remnant_targets.most_common(4)))
    # ---- 3. LEFTOVER census (reported)
    print('LEFTOVER: remnant reads of images last written by a scene op this frame %d; of images written by a remnant op %d; of images no op of the frame wrote (persistent) %d; remnant passes drawing over a scene-written surface %d'
          % (tot['leftover_reads'], tot['remnant_reads'], tot['persistent_reads'], sum(over_scene.values())))
    for r in res['leftover_reads_by_image'][:6]:
        print('     leftover read: %dx%d fmt %d by %s: %d' % (r['w'], r['h'], r['fmt'], r['effect'], r['n']))
    if a.json:
        os.makedirs(os.path.dirname(os.path.abspath(a.json)), exist_ok=True)
        json.dump(res, open(a.json, 'w'), indent=1)
    print('scene_skip_cut: %s' % ('FAIL (%d)' % bad if bad else 'PASS'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
