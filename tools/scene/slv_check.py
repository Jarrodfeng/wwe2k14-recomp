#!/usr/bin/env python3
r"""S103 lane SLV (small stream-free GPU levers): the proof. 0 game runs, 0 Surface runs; offline rigs only.

  py -I tools\scene\slv_check.py --part unit|wiring|post|plane [--root DIR]

Three invisible levers of the stream-free build (docs\research\s103\PO_post_colour.md section 5, levers 3, 2, 5):
  1 combine_lite on     start_settings.cpp stores rdr::g_combine_lite (the cvar wwe2k14_rdr_combine_lite, the bundle sets 1); post_pass.cpp then picks the no-bloom permutation on dead-bloom frames.
  2 depth-convert pass  NOT removed (see the hand-back: the consumers are the game's translated shaders and read the game's depth; census only: [rdr][post_levers] depth-convert passes).
  3 blank plane rule    live_map_plane.cpp leaves the plane chain out of a frame whose weight (fade[9]) is 0 with a real fade distance (plane_blank_rule.h), as the stream build's skip_blank_plane does.
Parts:
  unit    tools\scene\slv_unit.cpp (the rule's cases) + three planted twins on a mutated copy of the header (each must make the unit exit 1)
  wiring  text lints: both levers stored at start in the stream-free block (and READ-ONLY-ONCE marked), the blank rule sits beside plane_nodraw, the bundle sets both switches, the counters are printed
  post    post_rig.exe on brock_losswin frames: a dead-bloom row (the TEST row) at combine_lite 0 and 1: pictures max diff 0, the no-bloom permutation count > 0 and bloom clears skipped > 0 only with the lever
          (the lever-off twin shows the old counts: 0), and a natural row (no bloom) is unchanged. Depth-convert passes printed (lever 2: unchanged by design).
  plane   frame_rig on the run27 plane fixture (l9_check's told file): the rule on at the game's own weight is picture diff 0 against off with 0 blank frames; with the weight told 0 the rule skips the chain
          (blank frames > 0, passes skipped > 0) and the picture equals the no-plane-reader picture (a skipped chain = a plain mat); the lever-off twin shows the old count (blank 0).
Exit 0 = pass."""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
FAILS = []


def say(ok, text):
    print(('PASS  ' if ok else 'FAIL  ') + text)
    if not ok:
        FAILS.append(text)


def read(rel):
    with open(os.path.join(ROOT, rel), encoding='utf-8', errors='replace') as f:
        return f.read()


def run(cmd, env=None, timeout=1800):
    p = subprocess.run(cmd, capture_output=True, text=True, errors='replace', timeout=timeout, env=env, cwd=ROOT)
    return p.returncode, (p.stdout or '') + (p.stderr or '')


def scratch():
    d = os.path.join(os.environ.get('TEMP', ROOT), 'slv_check')
    os.makedirs(d, exist_ok=True)
    return d


# ---- unit ------------------------------------------------------------------------------------------------------------------------------------------

PLANTS = [
    ('blank at any matrix (the corner count is not looked at)', 'PlaneSquareCornersBehind(m4, y0) == 4', 'true'),
    ('w = 0 counted as in front (the sentinel fit is not blank)', '!(m4[12] * cx[k] + m4[13] * y0 + m4[14] * cz[k] + m4[15] > 0.f)', '(m4[12] * cx[k] + m4[13] * y0 + m4[14] * cz[k] + m4[15] < 0.f)'),
    ('one corner behind is enough (the game draws that case, clipped)', 'PlaneSquareCornersBehind(m4, y0) == 4', 'PlaneSquareCornersBehind(m4, y0) >= 1'),
]


def part_unit():
    d = scratch()
    exe = os.path.join(d, 'slv_unit.exe')
    inc = ['-I', os.path.join(ROOT, 'src', 'renderer_scene')]
    rc, text = run([CLANG, '-std=c++20'] + inc + [os.path.join(ROOT, 'tools', 'scene', 'slv_unit.cpp'), '-o', exe])
    say(rc == 0, 'slv_unit builds' + ('' if rc == 0 else ': ' + text[-300:]))
    if rc != 0:
        return
    rc, text = run([exe])
    say(rc == 0 and 'slv_unit: PASS' in text, 'slv_unit: the rule\'s cases pass (%d lines ok)' % text.count('ok  '))
    header = read('src/renderer_scene/plane_blank_rule.h')
    for i, (name, old, new) in enumerate(PLANTS):
        if old not in header:
            say(False, 'planted twin %d (%s): the shipped header no longer holds the text to mutate' % (i + 1, name))
            continue
        pd = os.path.join(d, 'plant%d' % (i + 1))
        os.makedirs(pd, exist_ok=True)
        with open(os.path.join(pd, 'plane_blank_rule.h'), 'w', encoding='utf-8') as f:
            f.write(header.replace(old, new, 1))
        pexe = os.path.join(pd, 'slv_unit.exe')
        rc, text = run([CLANG, '-std=c++20', '-I', pd] + inc + [os.path.join(ROOT, 'tools', 'scene', 'slv_unit.cpp'), '-o', pexe])
        if rc != 0:
            say(False, 'planted twin %d (%s) does not build: %s' % (i + 1, name, text[-200:]))
            continue
        rc, text = run([pexe])
        say(rc == 1 and 'FAIL' in text, 'planted twin %d (%s) is caught by the unit (exit %d)' % (i + 1, name, rc))


# ---- wiring ----------------------------------------------------------------------------------------------------------------------------------------

def part_wiring():
    ss = read('src/start_settings.cpp')
    off_start = ss.find('#if !WWE2K14_STREAM()\n  // S103 GT')
    block = ss[off_start:ss.find('// START-ONLY-END', off_start)] if off_start >= 0 else ''
    say('g_combine_lite.store(' in block and 'wwe2k14_rdr_combine_lite' in block, 'start_settings.cpp stores rdr::g_combine_lite from wwe2k14_rdr_combine_lite inside the stream-free START-ONLY block')
    say('g_skip_blank_plane.store(' in block and 'wwe2k14_rdr_skip_blank_plane' in block, 'start_settings.cpp stores rdr_scene::g_skip_blank_plane from wwe2k14_rdr_skip_blank_plane inside the stream-free START-ONLY block')
    say('REXCVAR_DEFINE_INT32(wwe2k14_rdr_combine_lite, 0,' in ss and 'REXCVAR_DEFINE_BOOL(wwe2k14_rdr_skip_blank_plane, false,' in ss, 'both cvars are defined in the stream-free block with the stream build\'s defaults (R8 compares them)')
    rd = read('src/renderer_adapter/reader.h')
    say('REXCVAR_DEFINE_INT32(wwe2k14_rdr_combine_lite, 0,' in rd and 'REXCVAR_DEFINE_BOOL(wwe2k14_rdr_skip_blank_plane, false,' in rd, 'reader.h defines the same two with the same defaults')
    bd = read('src/bundle3.h')
    say('X("wwe2k14_rdr_combine_lite", "1")' in bd and 'X("wwe2k14_rdr_skip_blank_plane", "true")' in bd, 'the shipped bundle sets both (so the shipped stream-free build gets them)')
    mp = read('src/renderer_scene/live_map_plane.cpp')
    i_nd, i_bl = mp.find('if (in.plane_nodraw)'), mp.find('PlaneChainBlank(m4, y0)')
    i_need = mp.find('if (!in.have_plane ||')
    i_mask = mp.find('fb.SetTargets(targets);')
    say(0 < i_nd < i_need < i_bl < i_mask, 'live_map_plane.cpp: the blank test (ground square all behind the camera) sits after the inputs check and ABOVE the mask pass')
    say('g_skip_blank_plane.load(' in mp[i_bl - 120:i_bl + 40] and 'PlaneFrameBlank' not in mp and 'fade[9]' not in mp, 'the rule is gated by the switch and does not read the weight (fade[9])')
    lm = read('src/renderer_scene/live_maps.cpp')
    say('blank-plane frames' in lm and 'passes skipped' in lm, 'the [scene][maps][e2] census line carries blank-plane frames and passes skipped')
    sh = read('src/renderer_host/scene_host.cpp')
    say(sh.count('LogPostLevers(') >= 3 and '[rdr][post_levers]' in sh, 'the host prints the post levers line (periodic and final)')
    pp = read('src/renderer_scene/post_pass.cpp')
    say('g_post_dconv_passes.fetch_add' in pp and 'g_post_bloom_clears_skipped.fetch_add' in pp, 'post_pass.cpp counts the depth-convert passes and the skipped bloom clears')


# ---- post ------------------------------------------------------------------------------------------------------------------------------------------

def post_setup(frames, capture):
    """Shared by the post parts: the rig, the proof tool, one test record per frame (the camera, the combine constants of the capture's own draw)."""
    sys.path.insert(0, HERE)
    sys.path.insert(0, os.path.join(ROOT, 'tools', 'fview'))
    import numpy as np
    import post_rig as pr
    fr = pr.fr
    main_root = fr.main_checkout()
    game = os.path.join(main_root, 'game_data', 'extracted')
    rig = os.path.join(ROOT, 'out', 'tools', 'post_rig.exe')
    proof = os.path.join(main_root, 'out', 'renderer', 'dev', 'post_proof.exe')
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import run_dirs  # S103 PSO: a private output folder per run (never a fixed path under the main checkout)
    work = run_dirs.run_out('SLV_post')
    os.makedirs(work, exist_ok=True)
    os.environ.setdefault('WWE_FVIEW_ALLOW_STALE', '1')
    from fview_exe import find_fview
    fview = find_fview(root=ROOT)
    for need in (rig, proof, game, capture):
        if not os.path.exists(need):
            say(False, 'cannot read its input: ' + need)
            return None
    env = dict(os.environ, WWE_RDR_VALIDATION='1')
    out = []
    for n in frames:
        rsc = os.path.join(capture, 'scene', 'frame_%06d.rsc' % n)
        dump = os.path.join(work, 'dump_%d.txt' % n)
        if not os.path.exists(dump):
            fr.run([proof, '--capture', capture, '--frame', str(n), '--dump', '--out', dump, '--root', ROOT], timeout=600)
        c = pr.parse_consts(open(dump, errors='replace').read()) if os.path.exists(dump) else None
        if not c or 'DOFParam' not in c:
            say(False, 'frame %d: no combine draw in the post_proof dump' % n)
            continue
        cam, why = fr.stream_camera(fview, capture, n, game, work)
        if cam is None:
            say(False, 'frame %d: no camera (%s)' % (n, why))
            continue
        vp, eye = cam[0], cam[1]
        natural = pr.make_row(c)
        dead = dict(c)   # the TEST row with the bloom permutation and both scales 0 (the entrance's dead-bloom frames: 1492 of 1664 combines in the stream run); a bloom row = the HDR look: the DoF depth is NEAREST
        dead['variant'] = c['variant'].replace('_fragment', '') + '_bloom'
        dead['BloomScale'], dead['StarScale'] = [0.0, 0, 0, 0], [0.0, 0, 0, 0]
        live = dict(dead)   # the same with the bloom LIVE (scale 0.4): the bloom chain runs, the combine fetches the glow
        live['BloomScale'] = [40.0, 0, 0, 0]   # (a strong scale: the bright pass keeps only what is above 0.96 of the blurred picture, a dark match frame holds little: any glow must show)
        lin = {k: v for k, v in c.items() if k not in ('BloomScale', 'StarScale')}   # the match's look: no bloom permutation = the DoF depth LINEAR (post_from_record: dof_depth_linear = !hdr). (This capture's own frames are bloom ones: nearest.)
        lin['variant'] = lin['variant'].replace('_bloom', '')
        rows = {'natural': pr.make_row(c), 'dead_bloom': pr.make_row(dead), 'live_bloom': pr.make_row(live), 'linear': pr.make_row(lin)}
        out.append((n, rsc, rows, vp, eye))
    return np, fr, rig, game, work, env, out


def rig_run(np, fr, rig, game, work, env, n, rsc, rname, row, vp, eye, tag, extra):
    rf = os.path.join(work, 'row_%s_%d.bin' % (rname, n))
    open(rf, 'wb').write(row)
    prefix = os.path.join(work, 'ours_%s_%s_%d' % (rname, tag, n))
    cmd = [rig, '--record', rsc, '--game', game, '--size', '1280x720', '--prefix', prefix, '--camera', ','.join('%.9g' % x for x in vp), '--eye', ','.join('%.9g' % x for x in eye), '--row', rf] + extra
    rc, text = run(cmd, env=env, timeout=1800)
    m = re.search(r'slv: combine_lite level (\d+), bloom clears skipped (\d+), depth-convert passes (\d+), depth-convert passes skipped (\d+)', text)
    nb = re.search(r'no-bloom permutation (\d+)', text)
    ver = re.search(r'validation_errors (\d+)', text)
    if rc != 0 or not m or not nb or not ver or int(ver.group(1)) != 0 or not os.path.exists(prefix + '_post.png'):
        say(False, 'frame %d %s %s: post_rig failed (exit %d): %s' % (n, rname, tag, rc, text[-300:].replace('\n', ' ')))
        return None
    return dict(level=int(m.group(1)), clears=int(m.group(2)), dconv=int(m.group(3)), skipped=int(m.group(4)), nobloom=int(nb.group(1)), img=fr.load(prefix + '_post.png'))


def maxdiff(np, a, b):
    return int(np.abs(a['img'].astype(np.int16) - b['img'].astype(np.int16)).max())


def part_post(frames, capture):
    st = post_setup(frames, capture)
    if not st:
        return
    np, fr, rig, game, work, env, items = st
    for n, rsc, rows, vp, eye in items:
        res = {}
        for rname in ('natural', 'dead_bloom', 'linear'):
            # combine_lite 0 / 1 (the SLV lever), each with the depth fold ON (the default); then lite 1 with the fold OFF (the dconv_ pass as before): the fold's twin
            for tag, extra in (('L0', ['--combine-lite', '0']), ('L1', ['--combine-lite', '1']), ('L1F0', ['--combine-lite', '1', '--depth-fold', '0'])):
                if (rname == 'natural' and tag == 'L1F0') or (rname == 'linear' and tag == 'L0'):
                    continue
                r = rig_run(np, fr, rig, game, work, env, n, rsc, rname, rows[rname], vp, eye, tag, extra)
                if r is None:
                    break
                res[(rname, tag)] = r
            else:
                continue
            res = None
            break
        if not res:
            continue
        for rname in ('natural', 'dead_bloom'):
            a, b = res[(rname, 'L0')], res[(rname, 'L1')]
            say(maxdiff(np, a, b) == 0, 'frame %d %s: combine_lite 1 vs 0 picture max diff %d (bar 0); depth-convert passes recorded %d -> %d, skipped by the fold %d -> %d' % (n, rname, maxdiff(np, a, b), a['dconv'], b['dconv'], a['skipped'], b['skipped']))
        a, b = res[('dead_bloom', 'L0')], res[('dead_bloom', 'L1')]
        say(a['nobloom'] == 0 and a['clears'] == 0, 'frame %d lever-off twin (combine_lite 0, dead-bloom row): no-bloom permutation %d, bloom clears skipped %d (the old counts, both 0)' % (n, a['nobloom'], a['clears']))
        say(b['nobloom'] >= 1 and b['clears'] >= 2, 'frame %d lever on (combine_lite 1, dead-bloom row): no-bloom permutation %d, bloom clears skipped %d (both above 0)' % (n, b['nobloom'], b['clears']))
        nat = res[('natural', 'L1')]
        say(nat['clears'] == 0, 'frame %d lever on, natural row: no clears skipped (%d): a frame with live bloom or no bloom permutation is not touched' % (n, nat['clears']))
        # the depth-convert fold (SLV2): the nearest (bloom / HDR) row folds, the linear-DoF (match) row keeps dconv_
        f_on, f_off = res[('dead_bloom', 'L1')], res[('dead_bloom', 'L1F0')]
        say(maxdiff(np, f_on, f_off) == 0 and f_on['skipped'] >= 1 and f_on['dconv'] == 0 and f_off['skipped'] == 0 and f_off['dconv'] >= 1,
            'frame %d depth fold, nearest row: fold on vs off picture max diff %d (bar 0); passes recorded %d vs %d, skipped %d vs %d (the pass is gone with the fold)' % (n, maxdiff(np, f_on, f_off), f_on['dconv'], f_off['dconv'], f_on['skipped'], f_off['skipped']))
        l_on, l_off = res[('linear', 'L1')], res[('linear', 'L1F0')]
        say(maxdiff(np, l_on, l_off) == 0 and l_on['skipped'] == 0 and l_on['dconv'] >= 1 and l_off['dconv'] == l_on['dconv'],
            'frame %d depth fold, linear-DoF row: unchanged, the pass is kept: picture max diff %d (bar 0), passes recorded %d (fold on) vs %d (off), skipped %d' % (n, maxdiff(np, l_on, l_off), l_on['dconv'], l_off['dconv'], l_on['skipped']))


def part_post_twin(frames, capture):
    """The planted twins of the post part's `picture max diff 0` claims: each MUST show a diff above 0 (a bar of 0 that cannot fail proves nothing)."""
    st = post_setup(frames, capture)
    if not st:
        return
    np, fr, rig, game, work, env, items = st
    for n, rsc, rows, vp, eye in items:
        base = rig_run(np, fr, rig, game, work, env, n, rsc, 'dead_bloom', rows['dead_bloom'], vp, eye, 'L1', ['--combine-lite', '1'])
        wrong = rig_run(np, fr, rig, game, work, env, n, rsc, 'dead_bloom', rows['dead_bloom'], vp, eye, 'L1Q', ['--combine-lite', '1', '--depth-fold-plant', '1'])
        if base is not None and wrong is not None:
            say(maxdiff(np, base, wrong) > 0 and wrong['skipped'] >= 1, 'frame %d planted twin, depth fold decoding with a wrong q: picture max diff %d (must be above 0; the fold ran: skipped %d)' % (n, maxdiff(np, base, wrong), wrong['skipped']))
        live0 = rig_run(np, fr, rig, game, work, env, n, rsc, 'live_bloom', rows['live_bloom'], vp, eye, 'L0', ['--combine-lite', '0'])
        live2 = rig_run(np, fr, rig, game, work, env, n, rsc, 'live_bloom', rows['live_bloom'], vp, eye, 'L1P2', ['--combine-lite', '1', '--combine-lite-plant', '2'])
        if live0 is not None and live2 is not None:
            print('  REPORT (not a bar) frame %d combine_lite plant 2 (the bloom is never fetched) on a live-bloom row: picture max diff %d: 0 here means these dark match frames have no glow above the 0.96 bright-pass threshold, so plant 1 (the DoF fetch) is the twin that bites' % (n, maxdiff(np, live0, live2)))
        p1 = rig_run(np, fr, rig, game, work, env, n, rsc, 'dead_bloom', rows['dead_bloom'], vp, eye, 'L1P1', ['--combine-lite', '1', '--combine-lite-plant', '1'])
        if base is not None and p1 is not None:
            say(maxdiff(np, base, p1) > 0, 'frame %d planted twin, combine_lite plant 1 (the DoF blur is never fetched): picture max diff %d (must be above 0)' % (n, maxdiff(np, base, p1)))
        live1 = rig_run(np, fr, rig, game, work, env, n, rsc, 'live_bloom', rows['live_bloom'], vp, eye, 'L1', ['--combine-lite', '1'])
        if live0 is not None and live1 is not None:
            say(maxdiff(np, live0, live1) == 0, 'frame %d live-bloom row, combine_lite 1 vs 0 (no plant): picture max diff %d (bar 0: a live bloom is not touched by the lever)' % (n, maxdiff(np, live0, live1)))


# ---- plane -----------------------------------------------------------------------------------------------------------------------------------------

def part_plane(frame, twin=False):
    sys.path.insert(0, HERE)
    import numpy as np
    from PIL import Image
    import l9_check as l9
    main_root = l9.MAIN
    cap = os.path.join(main_root, 'out', 'l9', 'run27', 'capture')
    if not os.path.isdir(os.path.join(cap, 'scene')):
        zp = os.path.join(main_root, 'out', 's103', 'run27_case.zip')
        if not os.path.exists(zp):
            say(False, 'the run27 case is not on disk: ' + zp)
            return
        cap = l9.unpack(zp, os.path.join(main_root, 'out', 'l9', 'run27'))
    base = os.path.join(main_root, 'out', 's103', 'SLV', 'plane')
    sp = l9.stream_plane(cap, frame, os.path.join(base, 'p27'))
    lines = open(sp['told']).read().splitlines()
    def variant(name, change):
        out = []
        for ln in lines:
            key = ln.split(' ', 1)[0]
            if key in change:
                v = ln.split()[1:]
                v = change[key](v)
                ln = key + ' ' + ' '.join(v)
            out.append(ln)
        pth = os.path.join(base, 'p27', 'told_plane_%s.txt' % name)
        open(pth, 'w').write('\n'.join(out) + '\n')
        return pth

    def w0(v):
        v[9] = '0.0'
        return v

    def sentinel(v):   # row 3 of the fit zeroed: M's w = 0 at every point, no corner of the ground square projects (the sentinel fit)
        v[12:16] = ['0.0'] * 4
        return v

    told_w0 = variant('weight0', {'plane_fade': w0})
    told_sent = variant('sentinel', {'plane_fit': sentinel, 'plane_fade': w0})
    nat_w = [float(x) for x in next(ln for ln in lines if ln.startswith('plane_fade ')).split()[1:]][9]
    print('  the game\'s own plane weight (fade[9]) in this frame: %g' % nat_w)

    def go(name, told, env):
        rc, text, rep, whole, _ = l9.rig(cap, frame, os.path.join(base, name), told, env)
        made = re.search(r'plane made (\d+) \(draws (\d+);', rep)
        blank = re.search(r'blank-plane frames (\d+), passes skipped (\d+)', rep)
        ok = rc == 0 and made and blank and os.path.exists(whole)
        if not ok:
            say(False, '%s: the rig failed (exit %d): %s' % (name, rc, (text or rep)[-300:].replace('\n', ' ')))
            return None
        return dict(made=int(made.group(1)), draws=int(made.group(2)), blank=int(blank.group(1)), passes=int(blank.group(2)), img=np.asarray(Image.open(whole).convert('RGB')).astype(np.int16))

    on, off = {'WWE_RDR_SKIP_BLANK_PLANE': '1'}, {'WWE_RDR_SKIP_BLANK_PLANE': '0'}
    runs = [('nat_off', sp['told'], off), ('nat_on', sp['told'], on),
            ('w0_off', told_w0, off), ('w0_on', told_w0, on),
            ('sent_off', told_sent, off), ('sent_on', told_sent, on), ('sent_noreader', told_sent, {'WWE_RDR_NO_PLANE_READER': '1'})]
    if twin:   # the planted twin of the plane part's `picture max diff 0` claims: what a WRONG skip looks like. A real shadow (the game's own weight and fit) with the chain left out = the no-plane-reader picture:
        # the comparison must see it (diff above 0), or a bar of 0 on "rule on vs off" proves nothing
        r = {k: go(k, t, e) for k, t, e in [('nat_off', sp['told'], off), ('nat_noreader', sp['told'], {'WWE_RDR_NO_PLANE_READER': '1'})]}
        if any(v is None for v in r.values()):
            return
        dd = int(np.abs(r['nat_off']['img'] - r['nat_noreader']['img']).max())
        say(dd > 0 and r['nat_off']['made'] > 0, 'planted twin: a live plane shadow left out (the no-plane-reader picture) differs from the drawn one: picture max diff %d (must be above 0), plane made %d' % (dd, r['nat_off']['made']))
        return
    r = {k: go(k, t, e) for k, t, e in runs}
    if any(v is None for v in r.values()):
        return
    d = lambda a, b: int(np.abs(r[a]['img'] - r[b]['img']).max())
    say(d('nat_off', 'nat_on') == 0 and r['nat_on']['blank'] == 0, 'the game\'s own weight (%g): rule on vs off picture max diff %d (bar 0), blank frames %d, plane made %d vs %d' % (nat_w, d('nat_off', 'nat_on'), r['nat_on']['blank'], r['nat_off']['made'], r['nat_on']['made']))
    say(d('w0_off', 'w0_on') == 0 and r['w0_on']['blank'] == 0 and r['w0_on']['made'] == r['w0_off']['made'] > 0,
        'weight told 0 over a REAL fit (the game\'s case b): the rule does NOT skip: blank frames %d, plane made %d vs %d, picture max diff %d (bar 0)' % (r['w0_on']['blank'], r['w0_off']['made'], r['w0_on']['made'], d('w0_off', 'w0_on')))
    say(r['sent_off']['blank'] == 0 and r['sent_off']['draws'] > 0, 'lever-off twin, sentinel fit: blank frames %d, mask draws %d (WWE_RDR_SKIP_BLANK_PLANE=0 is OFF: the mask still runs)' % (r['sent_off']['blank'], r['sent_off']['draws']))
    say(r['sent_on']['blank'] > 0 and r['sent_on']['passes'] == r['sent_on']['blank'] and r['sent_on']['draws'] == 0, 'rule on, sentinel fit: blank-plane frames %d, passes skipped %d (the mask only), mask draws %d (the mask is saved)' % (r['sent_on']['blank'], r['sent_on']['passes'], r['sent_on']['draws']))
    say(d('sent_off', 'sent_on') == 0 and d('sent_on', 'sent_noreader') == 0, 'sentinel fit: picture on = off = no-plane-reader (today\'s picture): max diffs %d / %d (bar 0)' % (d('sent_off', 'sent_on'), d('sent_on', 'sent_noreader')))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--part', required=True, choices=['unit', 'wiring', 'post', 'post_twin', 'plane', 'plane_twin'])
    ap.add_argument('--root', default=None)
    ap.add_argument('--frames', default='110,200')
    ap.add_argument('--capture', default=None)
    ap.add_argument('--plane-frame', type=int, default=500)
    a = ap.parse_args()
    if a.part == 'unit':
        part_unit()
    elif a.part == 'wiring':
        part_wiring()
    elif a.part in ('post', 'post_twin'):
        sys.path.insert(0, HERE)
        sys.path.insert(0, os.path.join(ROOT, 'tools', 'fview'))
        import frame_rig as fr
        cap = a.capture or os.path.join(fr.main_checkout(), 'out', 'captures', 'brock_losswin')
        (part_post if a.part == 'post' else part_post_twin)([int(x) for x in a.frames.split(',')], cap)
    else:
        part_plane(a.plane_frame, twin=a.part == 'plane_twin')
    if FAILS:
        print('FAIL  slv_check %s: %d' % (a.part, len(FAILS)))
        return 1
    print('PASS  slv_check ' + a.part)
    return 0


if __name__ == '__main__':
    sys.exit(main())
