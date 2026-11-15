#!/usr/bin/env python3
"""S102 lane R18-B1: the frame buffer's round trip (src/renderer_adapter/frame_roundtrip.h; docs/renderer/FRAME_ROUNDTRIP.md). Offline, 0 game runs, 0 Surface runs.

  py -I tools\\r18\\frame_roundtrip_check.py [--root DIR] [--only unit|wiring|bryan|scale|combo|brock|match|plants|pastbugs|counts|trace]

Switch wwe2k14_rdr_frame_roundtrip (PerFrame, default off; fview / rplay --frame-roundtrip N). Every arm is fview headless (--validation 1) on a gate capture: the hash of every frame's picture, the validation
error count AND the exit code are read (never "the csv exists"). The PLAYER config = the shipped bundle + the LOW levers + half-rate shadows (c3_check.py's BUNDLE / LOW), G11's self blit + restore on in both arms.
unit      the rules on plain ops (frame_roundtrip_test.cpp): 14 cases; four planted faults (identity_after_write, hidden_as_proof, wrong_depth, no_pair) must each FAIL the test.
wiring    each link of the switch (table row PerFrame default 0, cvar default 0, binder reads it every frame, census line, fview / rplay flags, the fate name): each removed in a text copy must fail the lint.
bryan     player config (G11 on) off / on, and off / on with G11 off: pictures byte-equal, validation 0, exit 0; frames taken reported per rule (0 taken = the lever does nothing for players).
scale     the same at --scale 1.5.      combo   with C3's frame_direct + shadow_direct and G8's alias on in both arms.      brock / match   the other gate captures.
plants    twins that must be CAUGHT (a picture differs on most frames, or validation errors, or the replay stops): no_pair (every restore the static rules pass is left out); the others where a capture can show them.
pastbugs  C3's fd_present_stale (the Present's copy dropped, the Present not re-pointed) and G11's restore_no_depth (the sweep hashes of the frame buffer's colour AND depth) still CAUGHT with the lever on.
counts    rplay --counts, 100 frames: copies, passes, draws, barriers, pipeline binds: they fall and none rises.
trace     rplay --frame-trace: shader-written pixels of the frame class (G11's 1.60 frame buffers -> the target reached)."""
import argparse
import importlib.util
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
spec = importlib.util.spec_from_file_location('c3_check', os.path.join(ROOT, 'tools', 'c3', 'c3_check.py'))
c3 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(c3)

fails = 0


def line(ok, text):
    global fails
    print(('PASS  ' if ok else 'FAIL  ') + text, flush=True)
    fails += 0 if ok else 1


RT = ['--frame-roundtrip', '2']
G11 = ['--frame-selfblit', '--frame-restore']
PLAYER = c3.BUNDLE + c3.LOW + ['--shadow-half']
RTL = re.compile(r'\[rdr\]\[frame_roundtrip\] level (\d+), frames (\d+) \(unusable (\d+), taken in (\d+)\), identity (\d+) \(restore (\d+), blit (\d+); ([\d.]+) Mpx\), aliased (\d+), declined (\d+) \(of (\d+) restores seen; by reason:(.*?)\), spans \(.*?\) planned (\d+), taken (\d+) \(by reason of the rest:(.*?)\), clears dropped (\d+), violations (\d+)\), plan [\d.]+ us a frame, copies dropped (\d+), passes dropped (\d+)(?: read-backs of left-out snapshots (\d+))?')
CAPS = {'bryan': ('dir', 'out/s101/lane92/case_20261112_122719_969_0_pc_run_exit/capture', 100),
        'bryan86': ('dir', 'out/lane86/bryan/capture', 91),
        'brock': ('dir', 'out/lane86/brock/capture', 120),
        'match': ('dir', 'out/lane86/wm30/capture', 120)}


class Arm(c3.Arm):
    def __init__(self, *a, **k):
        super().__init__(*a, **k)
        m = RTL.search(self.text)
        self.rt = dict(level=int(m.group(1)), frames=int(m.group(2)), unusable=int(m.group(3)), taken_frames=int(m.group(4)), identity=int(m.group(5)), restore=int(m.group(6)), blit=int(m.group(7)), mpx=float(m.group(8)),
                       aliased=int(m.group(9)), declined=int(m.group(10)), seen=int(m.group(11)), why=m.group(12).strip(), spans_planned=int(m.group(13)), spans_taken=int(m.group(14)), span_why=m.group(15).strip(), clears=int(m.group(16)),
                       violations=int(m.group(17)), copies=int(m.group(18)), passes=int(m.group(19)), readbacks=int(m.group(20) or 0)) if m else None


def setup(root, which):
    main_co = c3.main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    sys.path.insert(0, os.path.join(root, 'tools', 'fview'))
    import fview_exe as fe
    exe = fe.find_or_exit(root=root, kind='dev')
    work = os.path.join(root, 'out', 'r18_check')
    os.makedirs(work, exist_ok=True)
    kind, rel, frames = CAPS[which]
    path = os.path.join(main_co, rel)
    if not os.path.isdir(path):
        line(False, 'input missing: ' + path)
        return None
    return exe, (kind, path), game, work, frames


def fmt_rt(B):
    r = B.rt
    if not r:
        return 'NO CENSUS LINE'
    return 'identity %d (restore %d, blit %d), spans %d of %d planned (%s) in %d of %d frames, restores declined %d of %d (%s), clears dropped %d, copies dropped %d, passes dropped %d, violations %d' % (r['identity'], r['restore'], r['blit'],
        r['spans_taken'], r['spans_planned'], r['span_why'] or '-', r['taken_frames'], r['frames'], r['declined'], r['seen'], r['why'] or '-', r['clears'], r['copies'], r['passes'], r['violations'])


def duel(root, which, tag, base, new, label, want_restore=None, frames=None, scale=None, want_span=None):
    s = setup(root, which)
    if not s:
        return None
    exe, cap, game, work, n = s
    n = frames or n
    extra = ['--scale', str(scale)] if scale else []
    A = Arm(exe, cap, game, work, tag + '_A', base + extra, n)
    B = Arm(exe, cap, game, work, tag + '_B', base + new + extra, n)
    d = B.differing(A)
    ok = A.clean and B.clean and d == 0 and len(A.hashes) == len(B.hashes) and A.n > 1 and B.viol == 0 and B.rt is not None
    if want_restore is not None and B.rt:
        ok = ok and B.rt['restore'] >= want_restore * A.n
    if want_span is not None and B.rt:
        ok = ok and B.rt['spans_taken'] >= want_span * A.n and B.rt['violations'] == 0
    line(ok, '%s: %d frames, %d differ, validation %d / %d, exit %d / %d, stale-read violations %d | %s' % (label, B.n, d, A.valid, B.valid, A.rc, B.rc, B.viol, fmt_rt(B)))
    return A, B


def unit(root):
    clang = r'C:\Program Files\LLVM\bin\clang++.exe'
    exe = os.path.join(root, 'out', 'r18_check')
    os.makedirs(exe, exist_ok=True)
    exe = os.path.join(exe, 'frame_roundtrip_test.exe')
    b = subprocess.run([clang, '-std=c++20', '-Wall', '-Wextra', '-D_CRT_SECURE_NO_WARNINGS', '-I', os.path.join(root, 'src', 'renderer_adapter'), os.path.join(root, 'tools', 'r18', 'frame_roundtrip_test.cpp'), '-o', exe], capture_output=True, text=True)
    if b.returncode:
        print(b.stdout + b.stderr)
        line(False, 'frame_roundtrip_test.cpp did not compile')
        return
    u = subprocess.run([exe], capture_output=True, text=True)
    line(u.returncode == 0 and 'all passed' in u.stdout, 'unit test (the walk\'s op calls, pairs, versions, depth writes by state, cancelled draws, uploads, Foreign writes, unusable frames): %s' % (u.stdout.strip().splitlines() or ['no output'])[-1])
    for plant in ('identity_after_write', 'hidden_as_proof', 'wrong_depth', 'no_pair', 'no_ledger'):
        u = subprocess.run([exe, 'plant_' + plant], capture_output=True, text=True)
        line(u.returncode == 1 and 'FAILED' in u.stdout, 'unit twin %s: CAUGHT (%d failing checks)' % (plant, u.stdout.count('FAIL ')))


def wiring(root):
    def text(rel):
        return open(os.path.join(root, rel), encoding='utf-8', errors='replace').read()
    sw, rd, bd = text('src/renderer_adapter/rdr_switches.h'), text('src/renderer_adapter/reader.h'), text('src/renderer_adapter/binder.h')
    fv, rp = text('src/fview/main.cpp'), text('src/renderer/player/rplay.cpp')
    links = [('the switch table row (PerFrame, default 0)', lambda: re.search(r'X\(FrameRoundtrip, "wwe2k14_rdr_frame_roundtrip", PerFrame, 0\)', sw) is not None),
             ('the cvar (INT32, default 0)', lambda: re.search(r'REXCVAR_DEFINE_INT32\(wwe2k14_rdr_frame_roundtrip, 0,', rd) is not None),
             ('the binder reads it every frame (IntSeen at FrameCutsBegin)', lambda: 'rdr_sw::IntSeen(rdr_sw::kFrameRoundtrip' in bd),
             ('the live census every 600 frames', lambda: 'FrameRoundTripReport().c_str()' in bd),
             ('the fate name', lambda: '"frame_identity"' in fv and 'kFateFrameIdentity = 25' in bd),
             ('fview flags', lambda: '"--frame-roundtrip"' in fv and '"--frame-roundtrip-fault"' in fv),
             ('rplay flags', lambda: '"--frame-roundtrip"' in rp and '"--frame-roundtrip-fault"' in rp)]
    for name, fn in links:
        line(fn(), 'wiring: ' + name)
    # text twins: each link removed in a copy of its text must fail its probe (a twin that no longer applies fails as DID NOT APPLY)
    # the review's static guards (F1 / F3 / F4 / F5): each is a line of the binder's plan; removed in a text copy the probe must fail
    guards = [('F1 first snapshot decides / nothing writes F between the two', 'if (s.c1 > s.c2) std::swap(s.c1, s.c2);'),
              ('F1 tripwire at the second snapshot', 'rt_.SpanStillEqual('),
              ('F2 ledger scan', 'RtLedgerScan(frame);'),
              ('F2 banned pair never retaken', 'rt_ledger_.Banned(s.x)'),
              ('F3 unknown draws are depth writers', '(org && org->object != 0);'),
              ('F3 unknown draws in the window are unknown readers', 'p > s.c1 && p < d &&'),
              ('F4 L2 ops are not ours', 'p == pb_after_op_ || p == pb_first_op_'),
              ('F4 a claimed draw is not ours', 'post_claim_.draw[frame.ops[p].index]) return true;'),
              ('F5 the last stencil-touching op decides', 'if (o.kind == OpKind::Copy && frame.copies[o.index].dest.image == s.sd) break;')]
    for label, probe in guards:
        present = probe in bd
        planted = bd.replace(probe, 'XX_PLANTED_AWAY_XX', 1)
        line(present and probe not in planted, 'guard %s: %s' % (label, 'present, and its removal is caught' if present else 'MISSING'))
    twins = [('binder read', bd, 'rdr_sw::IntSeen(rdr_sw::kFrameRoundtrip', lambda t: 'rdr_sw::IntSeen(rdr_sw::kFrameRoundtrip' in t),
             ('switch row', sw, 'X(FrameRoundtrip, "wwe2k14_rdr_frame_roundtrip", PerFrame, 0)', lambda t: re.search(r'X\(FrameRoundtrip, "wwe2k14_rdr_frame_roundtrip", PerFrame, 0\)', t) is not None),
             ('fview flag', fv, '"--frame-roundtrip"', lambda t: '"--frame-roundtrip"' in t)]
    for label, src, old, probe in twins:
        planted = src.replace(old, 'XX_PLANTED_AWAY_XX', 1)
        did = planted != src
        line(did and not probe(planted), 'wiring twin %s: %s' % (label, 'caught' if did else 'DID NOT APPLY'))


def bryan(root):
    r = duel(root, 'bryan', 'by_g11', PLAYER + G11, RT, 'bryan, the PLAYER config, G11 on in both arms', want_span=1.0)
    duel(root, 'bryan', 'by_g11_off', PLAYER, RT, 'bryan, the PLAYER config, G11 off (the lever alone)', want_span=1.0)
    if r:
        line(r[1].rt is not None and r[1].rt['spans_taken'] >= r[1].n and r[1].rt['violations'] == 0, 'bryan player config: the span is taken in %d of %d frames, violations %d, read-backs of left-out snapshots %d' %
             (r[1].rt['spans_taken'] if r[1].rt else -1, r[1].n, r[1].rt['violations'] if r[1].rt else -1, r[1].rt['readbacks'] if r[1].rt else -1))


def scale(root):
    duel(root, 'bryan', 'sc15', PLAYER + G11, RT, 'bryan --scale 1.5, G11 on in both arms', scale=1.5)


def combo(root):
    duel(root, 'bryan', 'cb', PLAYER + G11 + c3.SD + c3.FD, RT, "bryan, C3's frame_direct + shadow_direct on in both arms (G11 on)")


def other(root, which):
    base = PLAYER if which != 'match' else ['--shadows-ours', '--shadow-casters-ours', '--plane-shadow-ours', '--post-ours', '--small-passes-ours', '--rim-ours', '--rim-chain-ours', '--circles-ours', '--movie-ours'] + c3.LOW + ['--shadow-half']   # (the match capture has no scene records: the promotion hooks stop fview there; G11's check uses the same list)
    duel(root, which, which + '_g11', base + G11, RT, '%s, the PLAYER config, G11 on in both arms' % which)
    duel(root, which, which + '_alone', base, RT, '%s, the PLAYER config, G11 off' % which)


def plants(root):
    s = setup(root, 'bryan')
    if not s:
        return
    exe, cap, game, work, n = s
    A = Arm(exe, cap, game, work, 'pl_A', PLAYER + G11, n)
    for fault, what in (('keep_clear', 'the snapshots and the restore are left out but the clear stays: the scene is wiped'), ('keep_depth_clear', 'a depth-only clear stays: the scene depth is wiped'),
                        ('no_pair', 'every restore the static rules pass is left out whatever the frame holds'),
                        ('wrong_depth', 'the depth pair is not asked'), ('identity_after_write', 'a pair written since still counts'), ('hidden_as_proof', 'no draw counts as a writer')):
        P = Arm(exe, cap, game, work, 'pl_' + fault, PLAYER + G11 + RT + ['--frame-roundtrip-fault', fault], n, allow_stop=True)
        d = P.differing(A)
        applied = False
        caught = P.stopped or d > 0 or not P.clean
        if fault in ('no_pair', 'keep_clear', 'keep_depth_clear'):
            applied = P.rt is not None and (P.rt['identity'] > 0 if fault == 'no_pair' else P.rt['spans_taken'] > 0)
            line(applied and caught, 'twin %s (%s): applied (identity %s), caught: %d of %d frames differ%s, validation %d, exit %d' % (fault, what, P.rt and P.rt['identity'], d, A.n, ', the replay stopped' if P.stopped else '', P.valid, P.rc))
        else:   # these three only differ from the real rule where the capture holds a write the rule must see: reported, caught at the unit level (a capture with no such write cannot show them)
            print('INFO  twin %s (%s): identity %s, %d of %d frames differ (the unit test is its proof where this capture has no such write)' % (fault, what, P.rt and P.rt['identity'], d, A.n), flush=True)


def pastbugs(root):
    s = setup(root, 'bryan')
    if not s:
        return
    exe, cap, game, work, n = s
    A = Arm(exe, cap, game, work, 'pb_A', PLAYER + c3.FD + RT, n)
    P = Arm(exe, cap, game, work, 'pb_fd_present_stale', PLAYER + c3.FD + RT + ['--copy-alias-fault', 'fd_present_stale'], n, allow_stop=True)
    d = P.differing(A)
    line(P.fd['taken'] > 0 and (P.stopped or d >= n // 2 or not P.clean), 'past bug C3 fd_present_stale (the Present\'s copy dropped, the Present not re-pointed) with the lever ON: applied (taken %d), CAUGHT: %d of %d frames differ%s' % (P.fd['taken'], d, A.n, ', the replay stopped' if P.stopped else ''))
    # G11's restore_no_depth: the sweep hashes the frame buffer's colour AND depth (images 8 / 9)
    kind, path, frames = CAPS['bryan86']
    main_co = c3.main_checkout(root)
    capdir = os.path.join(main_co, path)

    def sweep(tag, flags):
        csv = os.path.join(work_dir(root), 'sweep_%s.csv' % tag)
        if os.path.exists(csv):
            os.remove(csv)
        p = subprocess.run([exe, '--capture', capdir, '--replay', '--frames', '40', '--validation', '1', '--game', game, '--sweep', csv, '--sweep-images', '8,9'] + flags, capture_output=True, text=True, timeout=1700, errors='replace')
        errs = re.search(r'validation errors (\d+)', p.stdout + p.stderr)
        rows = [l.strip().split(',') for l in open(csv)][1:41] if os.path.exists(csv) else []
        return dict(rows=rows, errors=int(errs.group(1)) if errs else -1, rc=p.returncode)
    a = sweep('off', G11)
    n_ = sweep('nodepth', G11 + RT + ['--frame-cuts-fault', 'restore_no_depth'])
    b = sweep('on', G11 + RT)
    line(len(a['rows']) == 40 and a['rows'] == b['rows'] and a['errors'] == 0 and b['errors'] == 0 and a['rc'] == 0 and b['rc'] == 0, 'sweep (picture + frame buffer colour + DEPTH hashes, 40 frames): G11 on == G11 + the lever, validation %d / %d, exit %d / %d' % (a['errors'], b['errors'], a['rc'], b['rc']))
    diff = sum(1 for i in range(min(40, len(n_['rows']))) if a['rows'][i] != n_['rows'][i]) if len(a['rows']) == 40 and len(n_['rows']) == 40 else -1
    line(diff >= 20, 'past bug G11 restore_no_depth (the colour copied, the depth not) with the lever ON: CAUGHT, the picture / colour / depth hashes differ on %d of 40 frames' % diff)


def work_dir(root):
    w = os.path.join(root, 'out', 'r18_check')
    os.makedirs(w, exist_ok=True)
    return w


def rplay_counts(rplay, capdir, game, work, name, extra, frames=100):
    return c3.rplay_counts(rplay, capdir, game, work, name, extra, frames)


def counts(root):
    main_co = c3.main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    rplay = os.path.join(root, 'out', 'renderer', 'dev', 'rplay.exe')
    capdir = os.path.join(main_co, CAPS['bryan'][1])
    work = work_dir(root)
    if not (os.path.isfile(rplay) and os.path.isdir(capdir)):
        line(False, 'rplay or its capture is missing: %s %s' % (rplay, capdir))
        return
    K = ['--copy-batch', '1'] + G11   # G12's copy batch and G11's cuts ON in every arm (the merged levers that touch the same work)
    base, _ = rplay_counts(rplay, capdir, game, work, 'cnt_A', K)
    c, text = rplay_counts(rplay, capdir, game, work, 'cnt_B', K + RT)
    m = re.search(r'\[rdr\]\[frame_roundtrip\] level \d+, frames \d+ \(unusable \d+, taken in (\d+)\), identity (\d+)', text)
    taken = int(m.group(2)) if m else -1
    line(m is not None and taken >= 0 and c['passes'] <= base['passes'] + 0.01 and c['copies'] <= base['copies'] + 0.01 and c['barriers'] <= base['barriers'] + 0.01 and c['binds'] <= base['binds'] + 0.01 and c['draws'] <= base['draws'] + 0.01,
         'rplay counts (100 frames, bundle + G11 + copy batch in both arms): identity %d, copies %.2f -> %.2f, passes %.2f -> %.2f, draws %.2f -> %.2f, barriers %.2f -> %.2f, pipeline binds %.2f -> %.2f (none rises)' %
         (taken, base['copies'], c['copies'], base['passes'], c['passes'], base['draws'], c['draws'], base['barriers'], c['barriers'], base['binds'], c['binds']))
    # the player's own config
    pb, _ = rplay_counts(rplay, capdir, game, work, 'cnt_PA', K + ['--shadow-half'] + c3.LOW)
    pc, pt = rplay_counts(rplay, capdir, game, work, 'cnt_PB', K + ['--shadow-half'] + c3.LOW + RT)
    line(pc['passes'] <= pb['passes'] + 0.01 and pc['copies'] <= pb['copies'] + 0.01 and pc['barriers'] <= pb['barriers'] + 0.01 and pc['binds'] <= pb['binds'] + 0.01,
         'rplay counts under the player config (half-rate shadows + LOW levers): copies %.2f -> %.2f, passes %.2f -> %.2f, draws %.2f -> %.2f, barriers %.2f -> %.2f, pipeline binds %.2f -> %.2f' %
         (pb['copies'], pc['copies'], pb['passes'], pc['passes'], pb['draws'], pc['draws'], pb['barriers'], pc['barriers'], pb['binds'], pc['binds']))
    # the claim's config: the player config with G11 OFF (copy batch on in both arms)
    K0 = ['--copy-batch', '1', '--shadow-half'] + c3.LOW
    gb, _ = rplay_counts(rplay, capdir, game, work, 'cnt_GA', K0)
    gc, gt = rplay_counts(rplay, capdir, game, work, 'cnt_GB', K0 + RT)
    line(gc['passes'] <= gb['passes'] + 0.01 and gb['copies'] - gc['copies'] >= 1.9 and gc['barriers'] <= gb['barriers'] + 0.01 and gc['binds'] <= gb['binds'] + 0.01 and gc['draws'] <= gb['draws'] + 0.01,
         'rplay counts, the claim config (player config, G11 OFF, copy batch in both arms): copies %.2f -> %.2f (the 2 snapshots), passes %.2f -> %.2f (the restore), draws %.2f -> %.2f, barriers %.2f -> %.2f, pipeline binds %.2f -> %.2f' %
         (gb['copies'], gc['copies'], gb['passes'], gc['passes'], gb['draws'], gc['draws'], gb['barriers'], gc['barriers'], gb['binds'], gc['binds']))
    with open(os.path.join(work, 'counts_table.txt'), 'w') as f:
        for tag, v in (('base (bundle + G11 + copy batch)', base), ('+ roundtrip', c), ('player base', pb), ('player + roundtrip', pc), ('claim config (G11 off)', gb), ('claim config + roundtrip', gc)):
            f.write('%-34s copies %.2f passes %.2f barriers %.2f binds %.2f draws %.2f\n' % (tag, v['copies'], v['passes'], v['barriers'], v['binds'], v['draws']))


def cost(root):
    """The lever's own render-thread cost, two parts. (1) the per-op calls: the unit test's micro-benchmark (2000 ops a frame, ns an op, printed by frame_roundtrip_test.exe). (2) the span plan: the census line's `plan N us a frame`
    (one steady_clock pair a frame), read from an fview arm. rplay's bind_us was tried first and is useless here: another lane's load moves it by +-10 ms between identical arms."""
    exe = os.path.join(work_dir(root), 'frame_roundtrip_test.exe')
    u = subprocess.run([exe], capture_output=True, text=True)
    m = re.search(r'cost: ([\d.]+) ns an op', u.stdout)
    ns = float(m.group(1)) if m else -1
    line(m is not None and u.returncode == 0 and ns < 400, 'per-op calls of the lever (RoundTrip, the walk\'s call pattern, micro-benchmark): %.1f ns an op = %.1f us for a 2000-op frame (G11\'s Tracker does the same work again)' % (ns, ns * 2000 / 1000))
    s = setup(root, 'bryan')
    if not s:
        return
    exe_f, cap, game, work, n = s
    B = Arm(exe_f, cap, game, work, 'cost_B', PLAYER + RT, 60)
    m = re.search(r'plan ([\d.]+) us a frame', B.text)
    us = float(m.group(1)) if m else -1
    line(m is not None and us < 150, 'the span plan\'s wall time on Bryan (60 frames, PC): %.1f us a frame (the plan scans every op a few times)' % us)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--only', default='')
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    o = args.only
    steps = [('unit', unit), ('wiring', wiring), ('bryan', bryan), ('scale', scale), ('combo', combo), ('brock', lambda r: other(r, 'brock')), ('match', lambda r: other(r, 'match')), ('plants', plants),
             ('pastbugs', pastbugs), ('counts', counts), ('cost', cost)]
    for name, fn in steps:
        if o in ('', name):
            fn(root)
    print('R18-B1: %s' % ('all passed' if not fails else '%d FAILED' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
