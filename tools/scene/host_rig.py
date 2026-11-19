#!/usr/bin/env python3
r"""The scene host's headless rig gate (S103 landing 2, piece P7 H1; docs\renderer\P7_DESIGN.md section 5 H1 (b)). 0 game runs.

  py -I tools\scene\host_rig.py [--root DIR] [--captures DIR] [--tol 2] [--only a04_entrance] [--no-twins]

For each capture (default a04_entrance and brock_losswin under the main checkout's out\captures) and a few scene records of it:
  1. `host_rig.exe --mode sizes`: every present of the capture has one size, and the size read from fetch words equals the present's image record (the Tick's width / height rule);
  2. `--mode ref`:  LiveSceneDraw::DrawToPng of the record (the mirror path) at that size with the display curve -> ref_<frame>.png;
  3. `--mode host`: the SAME SceneHost the game runs, fed the record through its mailbox on a headless renderer; its presented image -> host_<frame>.png;
  4. per frame: max / mean absolute difference over RGB (0-255) and the number of pixels that differ. PASS when the max is <= --tol (owned_check's tolerance is far looser; the host
     path is the mirror's own drawing plus one copy and the present, so 0 is expected). The real numbers are printed, never rounded.
Planted twins (host mode, each must FAIL the comparison): stale_record (the previous record is drawn), skip_copy (F presented without the copy of `out`), wrong_size (the tick's width is
one pixel short). A twin that still passes means the comparison is blind: the gate fails.
S103 P7 tween (the decoupled 120 in the host; `--no-tween` skips it, `--tween-only` runs nothing else): for each capture a pair of records with motion (TWEEN_PAIRS) is run through the SAME host with HostOptions::tween_wanted on (`--tween`: the
list is run twice, the second pass is measured). The halfway frame's constants are checked against the rig's OWN expectation (every keyed draw found in both ticks: each blend range = (previous + this) / 2, every
byte outside the mixed ranges = this tick's), at least 90 % of the keyed draws must pair and some range must really move; the halfway picture must lie between the previous and this tick's picture (it is closer to
each than they are to each other, and differs from both); this tick's FINAL picture must equal the same host run without the tween (tolerance --tol). Planted twins (each must FAIL): alpha0 (weight 0: the halfway
is the previous tick), swap (this tick's own picture presented before the halfway one: the final differs), no_forget (a camera cut does not forget the previous tick). A clean `--cut` run must NOT blend into the cut record.
Rig-only fixtures are in host_rig.cpp's header (synthesised camera, body scale 1, DevForce): the same for both modes.
Exit 0 = pass, 1 = fail."""
import argparse
import os
import re
import subprocess
import sys
import io
import threading
from concurrent.futures import ThreadPoolExecutor

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
FRAMES = {'a04_entrance': [20, 60, 100], 'brock_losswin': [58, 120, 200]}
RAWID_CAUGHT = []   # captures whose raw-id twin was caught (at least one must be: an unnamed-object capture cannot show it)
TWEEN_PAIRS = {'a04_entrance': (60, 64), 'brock_losswin': (120, 160)}   # two records with motion (constants that really move between them)


def main_checkout(root):
    env = os.environ.get('RDR_MAIN_CHECKOUT')
    if env:
        return env
    common = subprocess.run(['git', '-C', root, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True).stdout.strip()
    return os.path.dirname(common) if common else root


# S103 GSP: the arms are independent processes (each its own --out dir): they run in parallel, bounded by PROC_SEM; each task prints into its own buffer and the buffers are
# joined in the serial order, so the verdict text is identical to a --serial run.
PROC_SEM = threading.BoundedSemaphore(1)
_TL = threading.local()


class _TlsOut:
    def __init__(self, real):
        self.real = real

    def write(self, t):
        b = getattr(_TL, 'buf', None)
        return (b if b is not None else self.real).write(t)

    def flush(self):
        pass


def buffered(fn, *args):
    """Run fn with stdout captured to a buffer (this thread only); returns (text, result)."""
    _TL.buf = io.StringIO()
    try:
        r = fn(*args)
        return _TL.buf.getvalue(), r
    finally:
        _TL.buf = None


class _Excl:
    """Readers / one writer: an exclusive run (the gpu stamps measurement: its pass-time numbers and stamp counts are wall-clock measurements, which a parallel arm on the same GPU disturbed once in a
    twin run) waits for every running arm to finish and holds new ones back while it runs."""
    def __init__(self):
        self.cv = threading.Condition()
        self.readers = 0
        self.writer = False
        self.writers_waiting = 0

    def enter(self, excl):
        with self.cv:
            if excl:
                self.writers_waiting += 1
                while self.writer or self.readers:
                    self.cv.wait()
                self.writers_waiting -= 1
                self.writer = True
            else:
                while self.writer or self.writers_waiting:
                    self.cv.wait()
                self.readers += 1

    def leave(self, excl):
        with self.cv:
            if excl:
                self.writer = False
            else:
                self.readers -= 1
            self.cv.notify_all()


EXCL = _Excl()


def run(cmd, timeout=1800, excl=False):
    EXCL.enter(excl)
    try:
        with PROC_SEM:
            p = subprocess.run(cmd, capture_output=True, text=True, errors='replace', timeout=timeout,
                               env=dict(os.environ, WWE_RIG_DEADLINE_S=os.environ.get('WWE_RIG_DEADLINE_S') or str(max(30, int(timeout) - 20))))   # S103 GST: the rig's own checked waits + deadline + heartbeat (host_rig.cpp) apply in the gate too, as in gstp_check.py
    finally:
        EXCL.leave(excl)
    return p.returncode, (p.stdout or '') + (p.stderr or '')


def compare(a_path, b_path):
    a = np.asarray(Image.open(a_path).convert('RGB')).astype(np.int16)
    b = np.asarray(Image.open(b_path).convert('RGB')).astype(np.int16)
    if a.shape != b.shape:
        return dict(max=255, mean=255.0, differ=a.shape[0] * a.shape[1], note='size %s vs %s' % (a.shape[:2], b.shape[:2]), lum=float(a.mean()))
    d = np.abs(a - b)
    return dict(max=int(d.max()), mean=float(d.mean()), differ=int((d.max(axis=2) > 0).sum()), note='', lum=float(a.mean()))


# S103 GSP review bug 1: a planted twin is CAUGHT only by its own failure line. A non-zero exit with an infrastructure line (the host never started, an asset load timed out, the host did not
# handle a tick, a record could not be read) is an ERROR, never a catch: parallel arms make those likelier, and a dead plant must not read as a caught one.
INFRA = re.compile(r'host_rig: (FAIL (renderer|host:|the host did not handle tick|the file index|record \d+|rows |capture frame \d+|no presented picture)|cannot open capture)')
OWN = re.compile(r'NOT DRAWN|host_rig: FAIL (concurrent )?CaptureShown|walk check frame \d+ .*: FAIL')


def infra_failure(text):
    m = INFRA.search(text or '')
    return m.group(0) if m else None


def judge_nonzero(rc, text):
    """For a planted run that exited non-zero: ('caught', why) when the output holds a failure line a plant can cause, else ('error', why)."""
    inf = infra_failure(text)
    if inf:
        return 'error', 'host run failed for its own reasons (%s), exit %d: the plant was not judged' % (inf, rc)
    m = OWN.search(text or '')
    if m:
        return 'caught', 'host mode failed its own check (%s)' % m.group(0)
    return 'error', 'exit %d without any failure line of this plant (output tail: %s)' % (rc, (text or '').strip()[-200:].replace(chr(10), ' | '))


TICK = re.compile(r'^tween tick (\d+): half (\d) matched (\d+) keyed (\d+) pairs (\d+) floats (\d+) moving_floats (\d+) bad_floats (\d+) bad_outside (\d+) max_err (\S+) \| d_prev_cur (\S+) d_prev_half (\S+) d_half_cur (\S+) moving_px (\d+)')


def tween_ticks(text):
    out = {}
    for ln in text.splitlines():
        m = TICK.match(ln)
        if m:
            g = m.groups()
            out[int(g[0])] = dict(half=int(g[1]), matched=int(g[2]), keyed=int(g[3]), pairs=int(g[4]), floats=int(g[5]), moving=int(g[6]), bad_floats=int(g[7]), bad_outside=int(g[8]),
                                  max_err=float(g[9]), d_pc=float(g[10]), d_ph=float(g[11]), d_hc=float(g[12]), moving_px=int(g[13]))
    return out


def tween_census(text):
    m = re.findall(r'tween (\d+) \(draws (\d+), waiting (\d+), cut (\d+), cold (\d+)\)', text)
    return tuple(int(x) for x in m[-1]) if m else None


def tween_section(exe, name, cap, game, out, size, tol):
    """Returns the number of failures; prints every number."""
    a, b = TWEEN_PAIRS[name]
    scene = os.path.join(cap, 'scene')
    od = os.path.join(out, name + '_tween')
    common = ['--scene', scene, '--game', game, '--frames', '%d,%d' % (a, b), '--size', size]
    bad = 0
    os.makedirs(od + '_off', exist_ok=True)
    rc, text = run([exe, '--mode', 'host'] + common + ['--out', od + '_off'])
    if rc != 0:
        print('FAIL [%s] tween: the no-tween host run failed\n%s' % (name, text[-1200:]))
        return 1
    os.makedirs(od, exist_ok=True)
    rc, text = run([exe, '--mode', 'host', '--tween'] + common + ['--out', od])
    text_plain = text
    if rc != 0:
        print('FAIL [%s] tween: the tween host run failed\n%s' % (name, text[-1200:]))
        return 1
    ticks = tween_ticks(text)
    census = tween_census(text)
    print('[%s] tween census (tweens, draws mixed, waiting, cut, cold): %s' % (name, census))
    if census is None or census[0] < 2 or census[3] != 0:
        print('FAIL [%s] tween: expected >= 2 tweens and no cut, got %s' % (name, census))
        bad += 1
    for n in (a, b):
        t = ticks.get(n)
        if not t:
            print('FAIL [%s] tween: no measured tick %d' % (name, n))
            bad += 1
            continue
        probs = []
        if t['half'] != 1:
            probs.append('no halfway frame')
        if t['keyed'] == 0 or t['pairs'] < 0.9 * t['keyed']:
            probs.append('only %d of %d keyed draws found in the previous tick' % (t['pairs'], t['keyed']))
        if t['moving'] < 100:
            probs.append('no constant really moves (%d floats): the check would be blind' % t['moving'])
        if t['bad_floats'] or t['bad_outside']:
            probs.append('halfway constants are not the per-draw average of the two ticks (%d floats off, %d bytes outside the ranges changed, max err %g)' % (t['bad_floats'], t['bad_outside'], t['max_err']))
        if not (t['d_pc'] > 0.5 and t['d_ph'] > 0.01 and t['d_hc'] > 0.01 and t['d_ph'] < t['d_pc'] and t['d_hc'] < t['d_pc']):
            probs.append('the halfway picture does not lie between the two (d_prev_cur %.3f, d_prev_half %.3f, d_half_cur %.3f)' % (t['d_pc'], t['d_ph'], t['d_hc']))
        print('%s [%s] tween tick %d: matched %d of %d keyed, %d floats (%d moving), max err %g, d_prev_cur %.3f d_prev_half %.3f d_half_cur %.3f %s' % ('FAIL' if probs else 'PASS', name, n, t['pairs'], t['keyed'], t['floats'], t['moving'], t['max_err'], t['d_pc'], t['d_ph'], t['d_hc'], '; '.join(probs)))
        bad += 1 if probs else 0
        r = compare(os.path.join(od, 'host_%d.png' % n), os.path.join(od + '_off', 'host_%d.png' % n))
        ok = r['max'] <= tol
        print('%s [%s] tween tick %d: the FINAL picture equals the no-tween run: max diff %d, mean %.4f %s' % ('PASS' if ok else 'FAIL', name, n, r['max'], r['mean'], r['note']))
        bad += 0 if ok else 1
    # a camera cut: nothing may be blended into the cut record
    cd = od + '_cut'
    os.makedirs(cd, exist_ok=True)
    rc, text = run([exe, '--mode', 'host', '--tween', '--cut', str(b)] + common + ['--out', cd])
    t = tween_ticks(text).get(b)
    ok = rc == 0 and t is not None and t['half'] == 0 and (tween_census(text) or (0, 0, 0, 0, 0))[3] >= 1
    print('%s [%s] tween: a camera cut at record %d is not blended into (half %s, census %s)' % ('PASS' if ok else 'FAIL', name, b, t['half'] if t else '?', tween_census(text)))
    bad += 0 if ok else 1
    # review F1: the same objects with other string-blob offsets (each id re-pointed at a fresh copy of its text) still pair as many draws as the plain run
    plain = tween_ticks(text_plain).get(b)
    ri = od + '_reintern'
    os.makedirs(ri, exist_ok=True)
    rc, text = run([exe, '--mode', 'host', '--tween', '--reintern'] + common + ['--out', ri])
    t = tween_ticks(text).get(b)
    ok = rc == 0 and t is not None and plain is not None and t['half'] == 1 and t['pairs'] == plain['pairs'] and t['bad_floats'] == 0 and t['keyed'] > 0
    print('%s [%s] tween (review F1): objects re-interned in another order pair the same draws (%s of %s keyed, plain run %s)' % ('PASS' if ok else 'FAIL', name, t['pairs'] if t else '?', t['keyed'] if t else '?', plain['pairs'] if plain else '?'))
    bad += 0 if ok else 1
    # review F2: no card-bake draw is tagged (the census of the plain tween run)
    cen = re.findall(r'own_blend census: instanced_tagged (\d+) bake_seen (\d+) bake_tagged (\d+)', text_plain)
    inst, bake_seen, bake_tagged = (int(x) for x in cen[-1]) if cen else (-1, -1, -1)
    ok = bool(cen) and bake_tagged == 0
    print('%s [%s] tween (review F2): own tags census: instanced draws tagged %d, card-bake draws seen %d, card-bake draws tagged %d (must be 0)%s' % ('PASS' if ok else 'FAIL', name, inst, bake_seen, bake_tagged, '' if bake_seen else ' [no card bake in this capture: the twin below is not exercised here]'))
    bad += 0 if ok else 1
    # planted twins (review F1 rawid: raw Object::id key, with the re-intern; F2 bakepair: bake tagged like a member; F4 lookup: see main)
    for plant, extra in (('rawid', ['--reintern']), ('bakepair', [])):
        pd = od + '_plant_' + plant
        os.makedirs(pd, exist_ok=True)
        rc, text = run(broken([exe, '--mode', 'host', '--tween', '--tween-plant', plant] + extra + common + ['--out', pd]))
        t = tween_ticks(text).get(b)
        caught, why = False, ''
        if rc != 0:
            kind, why = judge_nonzero(rc, text)
            if kind == 'error':
                print('twin tween %s [%s]: ERROR (%s)' % (plant, name, why))
                bad += 1
                continue
            caught = True
        elif plant == 'rawid':
            if t and plain and (t['pairs'] < plain['pairs'] or t['pairs'] < 0.9 * max(t['keyed'], 1)):
                caught, why = True, 'only %d of %d keyed draws paired (plain run %d)' % (t['pairs'], t['keyed'], plain['pairs'])
                RAWID_CAUGHT.append(name)
            else:
                caught, why = True, 'not exercised in this capture (its keyed draws are unnamed objects: pairs unchanged); the twin must be caught in at least one capture'
        else:
            c2 = re.findall(r'own_blend census: instanced_tagged (\d+) bake_seen (\d+) bake_tagged (\d+)', text)
            if c2 and int(c2[-1][2]) > 0:
                caught, why = True, 'the card bake draws were tagged (%s)' % c2[-1][2]
            elif c2 and int(c2[-1][1]) == 0:
                why = 'UNPROVEN HERE: this capture has no card bake (bake_seen 0)'
                caught = True   # not a failure of the tree; reported above
        print('twin tween %s [%s]: %s (%s)' % (plant, name, 'caught' if caught else 'NOT CAUGHT', why))
        bad += 0 if caught else 1
    # planted twins
    for plant, extra in (('alpha0', []), ('swap', []), ('no_forget', ['--cut', str(b)])):
        pd = od + '_plant_' + plant
        os.makedirs(pd, exist_ok=True)
        rc, text = run(broken([exe, '--mode', 'host', '--tween', '--tween-plant', plant] + extra + common + ['--out', pd]))
        tk = tween_ticks(text)
        caught, why = False, ''
        if rc != 0:
            kind, why = judge_nonzero(rc, text)
            if kind == 'error':
                print('twin tween %s [%s]: ERROR (%s)' % (plant, name, why))
                bad += 1
                continue
            caught = True
        elif plant == 'alpha0':
            t = tk.get(b)
            if t and (t['bad_floats'] > 0 or t['d_ph'] < 0.5 * max(t['d_pc'], 1e-9)):
                caught, why = True, 'halfway constants off by %d floats, d_prev_half %.3f' % (t['bad_floats'], t['d_ph'])
        elif plant == 'swap':
            r = compare(os.path.join(pd, 'host_%d.png' % b), os.path.join(od + '_off', 'host_%d.png' % b))
            if r['max'] > tol:
                caught, why = True, 'final picture differs from the no-tween run (max diff %d)' % r['max']
        else:   # no_forget
            t = tk.get(b)
            if t and t['half'] == 1:
                caught, why = True, 'blended across the cut (matched %d)' % t['matched']
        print('twin tween %s [%s]: %s (%s)' % (plant, name, 'caught' if caught else 'NOT CAUGHT', why))
        bad += 0 if caught else 1
    return bad


# S103 GSP review bug 3: the ms-share rules of the stamps check judge wall-clock GPU time shares (role 'other' < 10 % of the work, the roles' bracket sum within 2 % of the csv GPU time). Another process on the
# same GPU (any gate's rig, Windows time-slicing) lands inside our brackets and breaks them, and the gate's GPU slots are shared PC-wide, so no in-rig lock can guard them. The gate therefore judges the
# STRUCTURAL rules only (steps recorded == closed by a written stamp, the stamp count identity, no pool overflow, the colour / frame roles named and non-zero, the stamps switched on); the ms-share rules
# run in measuring runs (`--stamps-ms`, with their two twins), where the PC is meant to be quiet. Cleaner than draining every GPU slot of every gate for the check (that stalls the whole PC for a number the
# gate does not need): the gate stays deterministic, and a measurement is still taken where a measurement belongs.
MS_RULES = ("role 'other' holds", 'UNSTAMPED: the roles')
STAMP_TWINS_STRUCTURAL = (('gpu_unbracketed', 'UNBRACKETED: steps recorded'), ('gpu_overflow', 'UNBRACKETED: the stamp pool overflowed'), ('gpu_ident', 'UNBRACKETED: the stamp count identity'),
                          ('gpu_unnamed', 'colour target images have no role'), ('gpu_off', 'fewer than 3 stamp-per-step frames'))
STAMP_TWINS_MS = (('gpu_unnamed', "role 'other' holds"), ('gpu_work_csv', 'UNSTAMPED: the roles'))


def stamp_fails(text):
    return [ln.split('gpu stamps check: FAIL', 1)[1].strip() for ln in text.splitlines() if 'gpu stamps check: FAIL' in ln]


def stamps_section(exe, name, common, od, twins, ms=False):
    """S103 GT: the stream-free host's per-pass GPU stamps on one record (PC numbers): every recorded step closed by a WRITTEN stamp, the stamp count identity (2 + uploads pair + closes + tail) holds, no pool overflow, colour and frame passes named
    (and, with ms=True, `other` small and the roles' sum equal to the csv). Five structural twins (gpu_unbracketed, gpu_overflow, gpu_ident, gpu_unnamed, gpu_off) + two ms twins (gpu_unnamed's 'other' share, gpu_work_csv), each must FAIL its own check.
    Every run here is exclusive inside this rig (excl=True: no sibling arm of this rig runs beside it)."""
    bad = 0
    sd = od + '_gpu_stamps'
    os.makedirs(sd, exist_ok=True)
    sc = list(common)
    sc[sc.index('--out') + 1] = sd
    fr = sc.index('--frames') + 1
    sc[fr] = sc[fr].split(',')[len(sc[fr].split(',')) // 2]   # one record: the middle one
    rc, text = run([exe, '--mode', 'host', '--gpu-stamps'] + sc, excl=True)
    line = next((ln for ln in text.splitlines() if ln.startswith('gpu stamps:')), '')
    fails = stamp_fails(text)
    note = ''
    ok = rc == 0 and 'gpu stamps check: PASS' in text
    if not ok and not ms and rc != 0 and fails and not infra_failure(text) and 'NOT DRAWN' not in text and all(any(f.startswith(r) for r in MS_RULES) for f in fails):
        ok = True   # only ms-share rules failed: not judged in the gate (a quiet-PC measuring run, --stamps-ms, judges them)
        note = ' [ms-share rules not judged here (run with --stamps-ms on a quiet PC): %s]' % '; '.join(f[:80] for f in fails)
    print('%s [%s] gpu stamps (PC numbers, record %s): %s%s' % ('PASS' if ok else 'FAIL', name, sc[fr], line[:900] if line else text[-600:], note))
    bad += 0 if ok else 1
    if twins:
        # every check the rig keeps has a twin that fails it: (plant, the words the failing check must print)
        for plant, want in STAMP_TWINS_STRUCTURAL + (STAMP_TWINS_MS if ms else ()):
            rc, text = run([exe, '--mode', 'host', '--plant', plant] + sc, excl=True)
            inf = infra_failure(text)
            caught = rc != 0 and not inf and 'gpu stamps check: FAIL' in text and want in text
            print('twin %s [%s]: %s' % (plant, name, 'caught (%s)' % want if caught else ('ERROR (host run failed for its own reasons: %s)' % inf if inf else 'NOT CAUGHT')))
            bad += 0 if caught else 1
    return bad


def base_chain(name, common, od, out, tol):
    """ref + host for one capture; returns (bad, worst)."""
    bad, worst = 0, 0
    rc, text = run([EXE[0], '--mode', 'ref'] + common)
    if rc != 0:
        print('FAIL [%s]: ref mode failed\n%s' % (name, text[-1500:]))
        return 1, 0
    rc, text = run([EXE[0], '--mode', 'host'] + common)
    for ln in text.splitlines():
        if ln.startswith('host frame') or ln.startswith('[host] frames'):
            print('   ' + ln[:260])
    if rc != 0:
        print('FAIL [%s]: host mode failed (a frame was not drawn)\n%s' % (name, text[-1500:]))
        return 1, 0
    for f in FRAMES[name]:
        r = compare(os.path.join(od, 'host_%d.png' % f), os.path.join(od, 'ref_%d.png' % f))
        ok = r['max'] <= tol and r['lum'] > 1.0
        worst = max(worst, r['max'])
        print('%s [%s] frame %d: max diff %d, mean diff %.4f, %d px differ, mean luminance %.1f %s' % ('PASS' if ok else 'FAIL', name, f, r['max'], r['mean'], r['differ'], r['lum'], r['note']))
        if not ok:
            bad += 1
    return bad, worst


def missing_input(name, common, od, out, tol):
    """S103 ONF: a record with a required input missing (bit 10) still presents the same 3D picture, and the bit is counted; compares with the ref pictures of base_chain."""
    bad = 0
    md = os.path.join(out, name + '_missing_input')
    os.makedirs(md, exist_ok=True)
    mc = list(common)
    mc[mc.index('--out') + 1] = md
    rc, text = run([EXE[0], '--mode', 'host', '--missing-input'] + mc)
    counted = re.search(r'\[scene\]\[ours_missing\].*record:.*bit 10:(\d+)', text) is not None
    if rc != 0 or not counted:
        print('FAIL [%s] missing input: host mode %s, bit 10 %s' % (name, 'failed' if rc else 'ok', 'counted' if counted else 'NOT counted'))
        return 1
    for f in FRAMES[name]:
        r = compare(os.path.join(md, 'host_%d.png' % f), os.path.join(od, 'ref_%d.png' % f))
        ok = r['max'] <= tol and r['lum'] > 1.0
        print('%s [%s] missing input (bit 10) frame %d: max diff %d' % ('PASS' if ok else 'FAIL', name, f, r['max']))
        if not ok:
            bad += 1
    return bad


def plant_one(name, plant, common, out, od, tol):
    pd = os.path.join(out, name + '_plant_' + plant)
    os.makedirs(pd, exist_ok=True)
    pc = list(common)
    pc[pc.index('--out') + 1] = pd
    rc, text = run(broken([EXE[0], '--mode', 'host', '--plant', plant] + pc))
    caught = False
    why = ''
    if rc == 0:
        for f in FRAMES[name]:
            r = compare(os.path.join(pd, 'host_%d.png' % f), os.path.join(od, 'ref_%d.png' % f))
            if r['max'] > tol:
                caught, why = True, 'frame %d max diff %d' % (f, r['max'])
                break
    else:
        kind, why = judge_nonzero(rc, text)
        if kind == 'error':
            print('twin %s [%s]: ERROR (%s)' % (plant, name, why))
            return 1
        caught = True
    print('twin %s [%s]: %s (%s)' % (plant, name, 'caught' if caught else 'NOT CAUGHT', why))
    return 0 if caught else 1


EXE = ['']
BREAK_PLANTS = [False]   # twin of bug 1 (host_rig_serial_twin.py): every planted run gets a scene folder that does not exist, so the host fails for its own reasons: the verdict must be ERROR, never `caught`


def broken(cmd):
    if BREAK_PLANTS[0] and '--scene' in cmd:
        cmd = list(cmd)
        cmd[cmd.index('--scene') + 1] = os.path.join(os.path.dirname(cmd[cmd.index('--out') + 1]), 'no_such_scene')
    return cmd


def capture_prepare(a, name, exe, caps, game, out):
    """Phase 1 of one capture: the size read. Returns (state, info): state 'stop' (no capture), 'fail', 'tween' (--tween-only) or 'ok'."""
    EXE[0] = exe
    cap = os.path.join(caps, name)
    if not os.path.isdir(os.path.join(cap, 'scene')):
        print('FAIL: no capture at %s' % cap)
        return 'stop', None
    rc, text = run([exe, '--mode', 'sizes', '--capture', cap])
    print('[%s] %s' % (name, text.strip().splitlines()[-1] if text.strip() else '(no output)'))
    m = re.search(r'distinct sizes: (\d+)x(\d+) \(x\d+\)(?!.*\d+x\d+ \(x)', text)
    if rc != 0 or not m:
        print('FAIL [%s]: the present size read (fetch words vs the image record) failed or the capture has more than one size' % name)
        return 'fail', None
    size = '%sx%s' % (m.group(1), m.group(2))
    if a.tween_only:   # the tween lane's own check: only the tween section (the base comparison and its twins are the P7 H1 lane's)
        return 'tween', dict(cap=cap, size=size)
    frames = ','.join(str(f) for f in FRAMES[name])
    od = os.path.join(out, name)
    os.makedirs(od, exist_ok=True)
    common = ['--scene', os.path.join(cap, 'scene'), '--game', game, '--out', od, '--frames', frames, '--size', size]
    return 'ok', dict(cap=cap, size=size, od=od, common=common)


PLANTS = ('stale_record', 'skip_copy', 'wrong_size', 'shot_no_curve', 'shot_late', 'stale_walks')


def expected_verdicts(a, name):
    """S103 GSP review bug 7: how many verdict lines of each arm one capture prints when every arm ran (a skipped / dropped arm must not read as a pass)."""
    n = {'frames': len(FRAMES[name]), 'missing-input': len(FRAMES[name]), 'stamps': 1}
    if not a.no_twins:
        n['stamps twins'] = len(STAMP_TWINS_STRUCTURAL) + (len(STAMP_TWINS_MS) if a.stamps_ms else 0)
        n['plant twins'] = len(PLANTS)
        if not a.no_tween and name in TWEEN_PAIRS:
            n['tween'] = 12
    return n


def count_verdicts(text, name):
    q = re.escape(name)
    pats = {'frames': r'^(PASS|FAIL) \[%s\] frame \d+:' % q, 'missing-input': r'^(PASS|FAIL) \[%s\] missing input \(bit 10\) frame \d+' % q, 'stamps': r'^(PASS|FAIL) \[%s\] gpu stamps \(PC' % q,
            'stamps twins': r'^twin gpu_\w+ \[%s\]:' % q, 'plant twins': r'^twin (%s) \[%s\]:' % ('|'.join(PLANTS), q), 'tween': r'^((PASS|FAIL) \[%s\] tween|twin tween \w+ \[%s\]:)' % (q, q)}
    return {k: len(re.findall(v, text, re.M)) for k, v in pats.items()}


def capture_rest(a, name, exe, info, game, out, pool, stamps):
    """Phase 3 of one capture (S103 GSP: its independent arms run in parallel, the text comes out in the serial order; the stamps arms ran first, alone, in phase 2: `stamps` = (text, bad)). Returns (bad, worst)."""
    EXE[0] = exe
    cap, size, od, common = info['cap'], info['size'], info['od'], info['common']
    f_tween = pool.submit(buffered, tween_section, exe, name, cap, game, out, size, a.tol) if (not a.no_twins and not a.no_tween and name in TWEEN_PAIRS) else None
    t_base, (bad, worst) = pool.submit(buffered, base_chain, name, common, od, out, a.tol).result()
    print(t_base, end='')
    plants = []
    f_miss = None
    if bad == 0:   # the plants and the missing-input run compare with the ref pictures of base_chain
        f_miss = pool.submit(buffered, missing_input, name, common, od, out, a.tol)
        if not a.no_twins:
            plants = [pool.submit(buffered, plant_one, name, pl, common, out, od, a.tol) for pl in PLANTS]
    if f_miss is not None:
        t, r = f_miss.result()
        print(t, end='')
        bad += r
    print(stamps[0], end='')
    bad += stamps[1]
    for f in plants + ([f_tween] if f_tween is not None else []):
        t, r = f.result()
        print(t, end='')
        bad += r
    if bad == 0:   # every arm passed: every verdict line must be there (an arm that did not run, or printed under another shape, fails here)
        want, got = expected_verdicts(a, name), count_verdicts(_TL.buf.getvalue(), name)
        short = {k: '%d of %d' % (got[k], v) for k, v in want.items() if got[k] != v}
        if short:
            print('FAIL [%s]: verdict lines missing or extra: %s' % (name, ', '.join('%s %s' % (k, v) for k, v in short.items())))
            bad += 1
        else:
            print('verdict lines [%s]: all there (%s)' % (name, ', '.join('%d %s' % (v, k) for k, v in want.items())))
    return bad, worst


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--captures', default='')
    ap.add_argument('--tol', type=int, default=2)
    ap.add_argument('--only', default='')
    ap.add_argument('--no-twins', action='store_true')
    ap.add_argument('--no-tween', action='store_true')
    ap.add_argument('--tween-only', action='store_true')
    ap.add_argument('--out', default='')
    ap.add_argument('--break-plants', action='store_true', help='TWIN ONLY: planted runs get a missing scene folder (the host fails for its own reasons): every planted twin must then read ERROR, never caught')
    ap.add_argument('--serial', action='store_true', help='one process at a time (the old order; the twin of the parallel run)')
    ap.add_argument('--workers', type=int, default=0, help='parallel host_rig.exe processes (default: GATE_GPU_PROCS when the gate gives the check a share, else min(8, cpu/4))')
    ap.add_argument('--stamps-ms', action='store_true', help='also judge the gpu stamps ms-share rules and run their twins (measuring runs on a quiet PC; the gate judges the structural rules only)')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    main_dir = main_checkout(root)
    caps = a.captures or os.path.join(main_dir, 'out', 'captures')
    game = os.path.join(main_dir, 'game_data', 'extracted')
    exe = os.path.join(root, 'out', 'tools', 'host_rig.exe')
    if not os.path.exists(exe):
        exe = os.path.join(main_dir, 'out', 'tools', 'host_rig.exe')
    if not os.path.exists(exe):
        print('FAIL: no host_rig.exe (pwsh -NoProfile -File tools\\scene\\build_host_rig.ps1)')
        return 1
    if not os.path.isdir(game):
        print('FAIL: no game files at ' + game)
        return 1
    out = a.out or os.path.join(root, 'out', 's103', 'p7h1', 'rig')
    BREAK_PLANTS[0] = a.break_plants
    global PROC_SEM
    PROC_SEM = threading.BoundedSemaphore(1 if a.serial else (a.workers or int(os.environ.get('GATE_GPU_PROCS') or 0) or max(1, min(8, (os.cpu_count() or 4) // 4))))
    sys.stdout = _TlsOut(sys.stdout)
    os.makedirs(out, exist_ok=True)
    names = [n for n in FRAMES if not a.only or n == a.only]
    bad = 0
    if not a.no_tween:   # review F4: no map-lookup matrix is in any vertex variant's own blend set (no GPU, no capture); the planted twin keeps them and must fail
        rc, text = run([exe, '--f4'])
        print('%s %s' % ('PASS' if rc == 0 else 'FAIL', text.strip().splitlines()[-1] if text.strip() else '(no output)'))
        blindly = re.search(r'(\d+) are marked blendable', text)
        if rc != 0 or not blindly or int(blindly.group(1)) == 0:
            print('FAIL: the F4 check found a lookup range in the blend set, or it is blind (no lookup field is marked blendable by the shader build)')
            bad += 1
        rc, text = run([exe, '--f4', '--tween-plant', 'lookup'])
        f4 = re.search(r'own_blend F4: .*?(\d+) of those are in the own blend set', text)   # the plant's own line: a lookup range really is in the set (exit 1 alone could be anything)
        caught = rc != 0 and f4 is not None and int(f4.group(1)) > 0
        print('twin tween lookup: %s (%s)' % ('caught' if caught else ('ERROR' if rc != 0 else 'NOT CAUGHT'), text.strip().splitlines()[-1] if text.strip() else ''))
        bad += 0 if caught else 1
    worst = 0
    pool = ThreadPoolExecutor(max_workers=64)   # threads only wait on processes; PROC_SEM bounds the processes
    # phase 1 (parallel): the size reads. phase 2 (serial, nothing else running: S103 GSP "stamps arms first": the exclusive arms drain the pool once, at the start): every capture's gpu stamps arms. phase 3 (parallel): the rest.
    preps = [(name, pool.submit(buffered, capture_prepare, a, name, exe, caps, game, out)) for name in names]
    prepared = []
    for name, jb in preps:
        text, (state, info) = jb.result()
        prepared.append((name, text, state, info))
    stamps = {}
    for name, text, state, info in prepared:
        if state == 'ok':
            stamps[name] = buffered(stamps_section, exe, name, info['common'], info['od'], not a.no_twins, a.stamps_ms)
    jobs = []
    for name, text, state, info in prepared:
        if state == 'ok':
            jobs.append((text, pool.submit(buffered, capture_rest, a, name, exe, info, game, out, pool, stamps[name])))
        elif state == 'tween':
            jobs.append((text, pool.submit(buffered, tween_section, exe, name, info['cap'], game, out, info['size'], a.tol)))
        else:
            jobs.append((text, state))
    for text, jb in jobs:
        sys.stdout.write(text)
        if jb == 'stop':
            return 1
        if jb == 'fail':
            bad += 1
            continue
        t2, res = jb.result()
        sys.stdout.write(t2)
        b2, w = res if isinstance(res, tuple) else (res, 0)
        bad += b2
        worst = max(worst, w)
    if not a.no_tween and not RAWID_CAUGHT:
        print('FAIL: the raw-id twin (review F1) was caught in no capture')
        bad += 1
    print('host rig: %d failed; worst max diff %d (tolerance %d)' % (bad, worst, a.tol))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
