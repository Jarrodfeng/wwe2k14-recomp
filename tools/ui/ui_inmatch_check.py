#!/usr/bin/env python3
r"""Lane 126 offline proof (0 game runs): the 2D draws of ENTRANCES and MATCHES are ours (docs\renderer\UI_OURS.md, section "In-match and entrance 2D").

  py -I tools\ui\ui_inmatch_check.py [--root DIR] [--no-build] [--quick]

fview (out\renderer\dev\fview.exe through the real seam, --ui-text-ours --ui-ours), every line PASS / FAIL, a missing input FAILS, exit 1 on any FAIL:
  1. picture: ours equals the stream's, pixel for pixel (max channel difference 0), at frames of the Bryan entrance, the WM30 entrance and the Brock entrance zip.
  2. census: fallback 0, mismatch 0, unjoined <= the per-capture allowance (the one solid-fill key whose image no pac holds, measured), and the sprite / text draws of those
     frames are ours in the --ops table (ops class sprite / text with fate ui_ours = every draw but the allowance), validation errors 0 on an 8-frame replay.
  3. plants (each must break an independent identity):
     wrong_texture   (--replace-fault unchecked_wrong_texture: the first joined texture for every sprite) the picture MOVES; checked, mismatch > 0 and the picture = the stream's.
     no_deep         (--replace-fault no_deep: the bg-pac tier never starts) the lightmap page pass stays unjoined: ours sprites FEWER than clean (Brock zip, Bryan).
     no_view_check   (the "no_u_sign" idea: a signed / biased view of the stream image must be refused) the unit scenario of ui_ours_test: see check 4.
  4. view: a Decide on a signed view of a joined image keeps the stream's draw; the plant no_view_check draws ours (ui_ours_test.exe synth: its synth_view line).
Pictures and logs go under the main checkout's out\s101\ui_inmatch\ ; nothing from the game is written elsewhere."""
import argparse
import glob
import os
import re
import subprocess
import sys
import time
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
RUNS = os.path.join('out', 's101', 'runs')
ZIPS = {   # kind -> (run folder, frames)
    'bryan': ('20261112_122700_s100_owner_bryan_entrance', (90, 150)),
    'wm30': ('20261112_131029_s100_owner_s90_wm30', (127, 300)),
    'brock': ('20261111_215201_s100_owner_s89_brock', (100, 250)),
}
kTier1 = 10                # kUiTier1Count (ui_ours_core.h): pacs indexed at start-up
DEEP_ENTRY_BOUND = 2000    # review F1 cost guard: entries the second tier may index (measured: bgEtc 814 + one arena's "*lightmap*" members, 3 for bg04)
ALLOW_UNJOINED = {'bryan': 1, 'wm30': 1, 'brock': 0}
ALLOW_FRAME = {('wm30', 127): 3}   # frame 127 of the WM30 zip: the two 512x512 lightmap pages stay unjoined (measured; joined by frame 300) [L: the scene record has not named the arena yet there]   # draws a frame whose image no pac holds (measured, see UI_OURS.md): kept and counted


def main_checkout(here):
    try:
        common = subprocess.run(['git', '-C', here, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return here


def run(cmd, timeout=900, env=None):
    e = dict(os.environ)
    if env:
        e.update(env)
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, env=e, errors='replace')
    return p.returncode, (p.stdout or '') + (p.stderr or '')


class Report:
    def __init__(self):
        self.fails = 0

    def line(self, ok, text):
        print(('PASS' if ok else 'FAIL') + '  ' + text, flush=True)
        self.fails += 0 if ok else 1
        return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--quick', action='store_true')
    args = ap.parse_args()
    rep = Report()
    root = os.path.abspath(args.root)
    main_co = main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    work = os.path.join(main_co, 'out', 's101', 'ui_inmatch')
    cache = os.path.join(work, 'idx')
    os.makedirs(cache, exist_ok=True)
    if not os.path.isdir(os.path.join(game, 'pac', 'menu')):
        rep.line(False, 'input missing: the game files ' + game)
        return 1
    caps = {}
    for kind, (run_name, _) in ZIPS.items():
        d = os.path.join(work, 'case_' + kind)
        cap = os.path.join(d, 'capture')
        if not os.path.isfile(os.path.join(cap, 'images.bin')):
            zs = glob.glob(os.path.join(main_co, RUNS, run_name, 'case_*.zip'))
            if not zs:
                rep.line(False, 'input missing: the %s case zip under %s' % (kind, os.path.join(main_co, RUNS, run_name)))
                return 1
            os.makedirs(d, exist_ok=True)
            zipfile.ZipFile(zs[0]).extractall(d)
        if not os.path.isfile(os.path.join(cap, 'images.bin')):
            rep.line(False, 'the %s zip has no capture\\images.bin' % kind)
            return 1
        caps[kind] = cap
    t0 = time.time()
    if not args.no_build:
        rc, text = run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'fview', 'build.ps1')], 1800)
        if rc:
            print(text[-3000:])
            rep.line(False, 'fview build (exit %d)' % rc)
            return 1
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'fview')); import fview_exe as _fe   # lane 218: one locator, refuses a stale exe
    fexe = _fe.find_or_exit(root=root, kind='dev')
    if not os.path.isfile(fexe):
        rep.line(False, 'no ' + fexe)
        return 1
    try:
        import json
        import numpy as np
        from PIL import Image
    except Exception as e:
        rep.line(False, 'numpy / Pillow are needed to compare pictures: %s' % e)
        return 1
    env = {'WWE2K14_UI_INDEX_CACHE': cache}
    cache_pics = {}

    def fview(tag, kind, frame, extra, ops=False):
        key = (tag, kind, frame)
        if key in cache_pics:
            return cache_pics[key]
        d = os.path.join(work, 'arm_%s_%s_%d' % (tag, kind, frame))
        cmd = [fexe, '--capture', caps[kind], '--frame', str(frame), '--root', root, '--judge-out', d] + extra
        ops_json = os.path.join(d + '_ops.json')
        for attempt in range(10):   # the GPU is shared: only the frame-ring refusal is retried
            rc, text = run(cmd, 1200, env)
            if 'Not enough graphics memory for the frame ring' in text:
                print('      (shared GPU: frame ring refused, attempt %d; waiting)' % (attempt + 1), flush=True)
                time.sleep(20)
                continue
            break
        png = os.path.join(d, 'picture.png')
        if rc or not os.path.isfile(png):
            print(text[-2500:])
            raise RuntimeError('fview arm %s %s frame %d failed (exit %d)' % (tag, kind, frame, rc))
        img = np.asarray(Image.open(png).convert('RGB'), dtype=np.int16)
        draws = None
        if ops:   # (a second process: --ops and --judge-out do not share a run)
            rc2, text2 = run([fexe, '--capture', caps[kind], '--frame', str(frame), '--root', root, '--ops', '--json', ops_json] + extra, 1200, env)
            if rc2 or not os.path.isfile(ops_json):
                print(text2[-2000:])
                raise RuntimeError('fview ops arm %s %s frame %d failed (exit %d)' % (tag, kind, frame, rc2))
            draws = [o for o in json.load(open(ops_json, encoding='utf-8'))['ops'] if o.get('kind') == 'draw']
        cache_pics[key] = (img, text, draws)
        return cache_pics[key]

    def diff(a, b):
        d = np.abs(a - b).max(axis=2)
        return int(d.max()), int((d > 0).sum())

    census_re = re.compile(r'^\[rdr\]\[ui\] ours frames (\d+), fallback (\d+), replaced (\d+), kept (\d+) \(not ready (\d+), unjoined (\d+), mismatch (\d+)', re.M)

    def census(text):
        m = census_re.search(text)
        return tuple(map(int, m.groups())) if m else None

    deep_re = re.compile(r'deep \(bg lightmaps\) (\w+): (\d+) pacs walked \(arena (-?\d+)\), (\d+) failed, (\d+) pending, (\d+) entries, (\d+) ms')
    pacs_re = re.compile(r'index (\d+) textures in (\d+) pacs')

    def hook_line(text):
        m = re.search(r'^\[fview\] \[rdr\]\[ui\] (.*)$', text, re.M)
        return m.group(1) if m else ''

    def ours_2d(draws):
        sp = [o for o in draws if o['class'] in ('sprite', 'text') and o['origin_op'] == 19 and o['call_site'] in (0x82BC5688, 0x82BC3784, 0x82BC8BEC)]   # (the claimed sites; T_Rect 0x82BC3EE8 is untextured)
        return len([o for o in sp if o['fate'] in ('ui_ours', 'ui_text_ours')]), len(sp)

    ui = ['--ui-text-ours', '--ui-ours', '--game', game]
    ui1 = ['--ui-ours', '--game', game]   # (the plants name one hook: --replace-fault is parsed by every registered class)
    sel = {k: (v[1][:1] if args.quick else v[1]) for k, v in ZIPS.items()}
    for kind, frames in sel.items():
        for f in frames:
            s, _, _ = fview('stream', kind, f, [])
            o, text, draws = fview('clean', kind, f, ui, ops=True)
            mx, npx = diff(o, s)
            c = census(text)
            n_ours, n_2d = ours_2d(draws)
            kept = n_2d - n_ours
            ok = mx == 0 and c is not None and c[1] == 0 and c[6] == 0 and kept <= ALLOW_FRAME.get((kind, f), ALLOW_UNJOINED[kind]) and n_ours > 0 and 'ERRORS' not in text
            rep.line(ok, 'fview %s frame %d: ours vs stream max channel diff %d (%d pixels); fallback %s, mismatch %s; 2D draws %d, ours %d, kept %d (allowance %d)' % (
                kind, f, mx, npx, c[1] if c else '?', c[6] if c else '?', n_2d, n_ours, kept, ALLOW_FRAME.get((kind, f), ALLOW_UNJOINED[kind])))
            # review F4: the honest proof that the UI hook never writes geometry: every ui_ours draw's vertex and index bytes are the stream's (the binder's trace, geom_ours)
            ui_draws = [d for d in draws if d['fate'] == 'ui_ours']
            bad = [d for d in ui_draws if d.get('geom_ours', 1) != 0]
            rep.line(len(ui_draws) > 0 and not bad, 'fview %s frame %d: %d ui_ours draws, %d with a hook-supplied vertex / index byte (bar 0; plant no_u_sign is not applicable: keep_stream_geometry, the hook never writes vertices)' % (kind, f, len(ui_draws), len(bad)))
            # review F1 / F3: the second tier (bgEtc + the CURRENT arena's pac only), what it cost, and that it ran where the page pass is
            if kind in ('bryan', 'brock'):
                hl = hook_line(text)
                dm, pm = deep_re.search(hl), pacs_re.search(hl)
                if dm and pm:
                    state, walked, arena, failed, pending, entries, ms = dm.group(1), int(dm.group(2)), int(dm.group(3)), int(dm.group(4)), int(dm.group(5)), int(dm.group(6)), int(dm.group(7))
                    arena_pac = os.path.join(game, 'pac', 'bg', 'bg%02d.pac' % arena)
                    want_pacs = kTier1 + 1 + (1 if arena >= 0 and os.path.isfile(arena_pac) else 0)   # first tier + bgEtc + the arena's pac, computed from the disk
                    ok = state == 'done' and failed == 0 and pending == 0 and arena >= 0 and walked == want_pacs - kTier1 and int(pm.group(2)) == want_pacs and entries <= DEEP_ENTRY_BOUND
                    rep.line(ok, 'fview %s frame %d: second tier %s, %d pacs walked (arena %d), %d entries (bound %d), %d ms; index in %s pacs (expected %d, from the disk)' % (
                        kind, f, state, walked, arena, entries, DEEP_ENTRY_BOUND, ms, pm.group(2), want_pacs))
                else:
                    rep.line(False, 'fview %s frame %d: no "deep (bg lightmaps) ..." / "index N textures in P pacs" in the hook line: %s' % (kind, f, hl[:200]))
    for kind in ('bryan',):
        rc, vt = run([fexe, '--capture', caps[kind], '--frames', '8', '--validation', '1', '--replay', '--root', root] + ui, 1200, env)
        rep.line(rc == 0 and 'validation errors 0' in vt and 'validation active: yes' in vt, 'fview %s: 8-frame replay with the validation layer on, errors 0 (exit %d)' % (kind, rc))
    # plants
    for kind, f in (('bryan', sel['bryan'][0]), ('wm30', sel['wm30'][0])):
        s, _, _ = fview('stream', kind, f, [])
        clean, _, cd = fview('clean1', kind, f, ui1, ops=True)
        raw, _, _ = fview('raw_wt', kind, f, ui1 + ['--replace-fault', 'unchecked_wrong_texture'])
        chk, ctext, _ = fview('chk_wt', kind, f, ui1 + ['--replace-fault', 'wrong_texture'])
        moved = diff(raw, clean)[1]
        cm = census(ctext)
        rep.line(moved > 0 and cm is not None and cm[6] > 0 and diff(chk, s)[1] == 0,
                 'fview %s plant wrong_texture (frame %d): unchecked picture moves by %d pixels; checked: mismatch %s, pixels from the stream %d (bar 0)' % (kind, f, moved, cm[6] if cm else '?', diff(chk, s)[1]))
    for kind in ('bryan', 'brock'):
        f = sel[kind][0]
        _, _, cd = fview('clean1', kind, f, ui1, ops=True)
        _, _, pd = fview('nodeep', kind, f, ui1 + ['--replace-fault', 'no_deep'], ops=True)
        a, b = ours_2d(cd)[0], ours_2d(pd)[0]
        rep.line(b < a, 'fview %s plant no_deep (frame %d): the bg-pac tier never starts, ours 2D draws %d -> %d (the lightmap page pass stays the stream\'s)' % (kind, f, a, b))
    f = sel['bryan'][0]
    _, _, cd = fview('clean1', 'bryan', f, ui1, ops=True)
    _, wtext, wd = fview('wrongarena', 'bryan', f, ui1 + ['--replace-fault', 'deep_wrong_arena'], ops=True)
    a, b = ours_2d(cd)[0], ours_2d(wd)[0]
    rep.line(b < a, 'fview bryan plant deep_wrong_arena (frame %d): the neighbouring arena\'s pac is indexed, ours 2D draws %d -> %d (the arena\'s page sprites stay unjoined)' % (f, a, b))
    f = sel['wm30'][0]
    _, _, cd = fview('clean1', 'wm30', f, ui1, ops=True)
    _, _, td = fview('tierb', 'wm30', f, ui1 + ['--replace-fault', 'tier_boundary'], ops=True)
    a, b = ours_2d(cd), ours_2d(td)
    rep.line(b[0] < a[0] and (b[1] - b[0]) > ALLOW_UNJOINED['wm30'], 'fview wm30 plant tier_boundary (frame %d): bgEtc filtered by name (the boundary one pac low), ours 2D draws %d -> %d, kept %d > the allowance %d (the check above would FAIL)' % (f, a[0], b[0], b[1] - b[0], ALLOW_UNJOINED['wm30']))
    # the view check (the "no_u_sign" idea) is a unit scenario
    exe = os.path.join(root, 'out', 'tools', 'ui_ours_test.exe')
    if os.path.isfile(exe):
        for fault, want in (('', True), ('no_view_check', False)):
            cmd = [exe, 'synth', '--game', game, '--cache', cache] + (['--fault', fault] if fault else [])
            rc, text = run(cmd, 600)
            m = re.search(r'^(PASS|FAIL)  synth_view\s+(.*)$', text, re.M)
            rep.line(m is not None and (m.group(1) == 'PASS') == want, 'unit view %s: signed view keeps the stream\'s draw -> %s' % (fault or '(clean)', m.group(0)[:150] if m else 'no such line (exit %d)' % rc))
        # review F3 / F5: the unit mirrors the tiers (first tier only, then the trigger) on the Brock zip; the gpu_copy scenario aimed at the 512 x 512 page (fade_lightmap00)
        _, ctext, _ = fview('clean1', 'brock', sel['brock'][0], ui1)
        am = deep_re.search(hook_line(ctext))
        arena = am.group(3) if am else '-1'
        def unit(fault, extra):
            cmd = [exe, 'capture', '--game', game, '--cache', cache, '--capture', caps['brock'], '--kind', 'match', '--step', '10', '--arena', arena] + extra + (['--fault', fault] if fault else [])
            rc, text = run(cmd, 1800)
            return {m.group(2): (m.group(1) == 'PASS', m.group(3)) for m in re.finditer(r'^(PASS|FAIL)  (\S+)\s+(.*)$', text, re.M)}
        ln = unit('', [])
        bad = [k for k, v in ln.items() if not v[0]]
        rep.line(not bad and 'tier2_trigger' in ln and 'unjoined' in ln, 'unit brock (first tier, then the trigger, arena %s): %d checks, failing: %s | %s' % (arena, len(ln), ', '.join(bad) or 'none', ln.get('unjoined', (0, '?'))[1][:130]))
        ln = unit('no_deep', [])
        rep.line('unjoined' in ln and not ln['unjoined'][0], 'unit brock plant no_deep: the unjoined check FAILs (%s)' % ln.get('unjoined', (0, 'no such line'))[1][:110])
        ln = unit('', ['--copy-after', '150', '--copy-width', '512'])
        rep.line('gpu_copy' in ln and ln['gpu_copy'][0], 'unit brock gpu_copy aimed at the 512x512 page (fade_lightmap00): %s' % ln.get('gpu_copy', (0, 'no such line'))[1][:200])
        ln = unit('gpu_copy_after_join', ['--copy-after', '150', '--copy-width', '512'])
        rep.line('gpu_copy' in ln and not ln['gpu_copy'][0], 'unit brock plant gpu_copy_after_join (the feed that forgets): the gpu_copy check FAILs (%s)' % ln.get('gpu_copy', (0, 'no such line'))[1][:160])
    else:
        rep.line(False, 'no ' + exe + ' (build it: tools\\ui\\build_ui_ours_test.ps1)')
    print('%s  lane 126 ui_inmatch: %d failing check(s), %.0f s' % ('PASS' if not rep.fails else 'FAIL', rep.fails, time.time() - t0))
    return 1 if rep.fails else 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except RuntimeError as e:
        print('FAIL  ' + str(e))
        sys.exit(1)
