#!/usr/bin/env python3
"""S102 lane R24-C: stencil liveness (docs/renderer/DEPTH_NOSTENCIL.md). Offline, 0 game runs, 0 Surface runs.

  py -I tools\\r24c\\r24c_check.py [--root DIR] [--only unit|wiring|bryan|brock|match|wm30|plants|valid3|selftest|arms] [--frames N]

unit     out\\tools\\r24c_unit.exe: the census's stencil rule + 4 planted weakenings that must each fail it.
wiring   every link of the PerFrame switch wwe2k14_rdr_stencil_dead (table, cvar, stage_split packet, live.cpp per-frame read, the renderer's read, the three cuts) is in the source; each removed in a text copy must fail.
bryan / brock / match / wm30   fview headless, the player bundle: stencil_dead OFF vs ON, presented-surface hashes equal on EVERY frame, validation 0 AND exit code 0, the lever TOOK (counters > 0);
         the census per capture (Bryan / Brock / match: no stencil use, wm30: stencil volumes: the stencil capture, `stencil really used on` >= 1).
plants   (wm30, the stencil capture) plant 1 an absorbed stencil clear dropped before a stencil test: the picture must change; plant 2 every depth copy depth-only; plant 3 an in-pass stencil clear
         stripped although a later pass reads it. Reported by what they do; a plant that changes nothing in this capture is said so (INFO), never passed as a catch.
valid3   (wm30) --validation 3 (synchronization + best practices): 0 errors, ON.
Validation errors AND the exit code are read, never the csv's existence (G5 review)."""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import r24c_common as C

fails = 0


def line(ok, text):
    global fails
    print(('PASS  ' if ok else 'FAIL  ') + text, flush=True)
    fails += 0 if ok else 1


CENSUS = re.compile(r'\[r24c\] census: depth images (\d+), stencil really used on (\d+), stencil-tested with no effect on (\d+), stencil cleared on (\d+), non-zero stencil clears on (\d+)')
LEVER = re.compile(r'\[r24c\] stencil_dead: (\w+), plant (\d+), frames (\d+), absorbed clears dropped (\d+) kept (\d+), in-pass clears stripped (\d+) kept (\d+), depth copies depth-only (\d+) whole (\d+), stencil loads dead (\d+), poison (\d+), oracle: stencil-testing passes (\d+), undefined at the test (\d+)')


def census_of(run):
    m = CENSUS.search(run.text)
    return dict(zip(('images', 'used', 'tested', 'cleared', 'nonzero'), map(int, m.groups()))) if m else None


def lever_of(run):
    m = LEVER.search(run.text)
    if not m:
        return None
    d = dict(zip(('mode', 'plant', 'frames', 'abs_dropped', 'abs_kept', 'in_stripped', 'in_kept', 'copy_depth', 'copy_whole', 'loads_dead', 'poison', 'oracle_tests', 'oracle_bad'), m.groups()))
    return {k: (v if k == 'mode' else int(v)) for k, v in d.items()}


# ---- unit ---------------------------------------------------------------------------------------------------------------------------------------------------------------
def unit(root):
    r = subprocess.run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'r24c', 'build_r24c_unit.ps1')], capture_output=True, text=True, timeout=600)
    exe = os.path.join(root, 'out', 'tools', 'r24c_unit.exe')
    line(r.returncode == 0 and os.path.isfile(exe), 'unit test builds')
    if not os.path.isfile(exe):
        return
    r = subprocess.run([exe], capture_output=True, text=True, timeout=120)
    oks = sum(1 for l in r.stdout.splitlines() if l.startswith('ok'))
    line(r.returncode == 0 and 'all passed' in r.stdout and oks >= 12, 'unit test: the census stencil rule passes its %d expectations' % oks)
    for plant in ('no_back', 'always_ok', 'test_flag', 'ops_only'):
        r = subprocess.run([exe, '--plant', plant], capture_output=True, text=True, timeout=120)
        first = next((l for l in r.stdout.splitlines() if l.startswith('FAIL')), '')
        line(r.returncode == 1, 'unit plant %s is caught: %s' % (plant, first[:100] if first else 'NOT CAUGHT'))


# ---- wiring -------------------------------------------------------------------------------------------------------------------------------------------------------------
FILES = ('src/renderer_adapter/rdr_switches.h', 'src/renderer_adapter/reader.h', 'src/renderer_adapter/live.cpp', 'src/renderer_adapter/stage_split.h', 'src/renderer/renderer.cpp',
         'src/renderer/depth_nostencil.h', 'src/fview/main.cpp', 'src/renderer/player/rplay.cpp')


def lint(fs):
    """a list of the links that are missing (empty = wired)"""
    bad = []
    m = re.search(r'X\(StencilDead, "wwe2k14_rdr_stencil_dead", (\w+), (\d+)\)', fs['src/renderer_adapter/rdr_switches.h'])
    if not m or m.group(1) != 'PerFrame' or m.group(2) != '0':
        bad.append('table line StencilDead (PerFrame, default 0)')
    if 'REXCVAR_DEFINE_BOOL(wwe2k14_rdr_stencil_dead, false,' not in fs['src/renderer_adapter/reader.h']:
        bad.append('cvar default false')
    if 'stencil_dead = false' not in fs['src/renderer_adapter/stage_split.h']:
        bad.append('stage_split Packet field')
    lv = fs['src/renderer_adapter/live.cpp']
    if 'rdr_sw::BoolSeen(rdr_sw::kStencilDead, rdr::g_stencil_dead.load(std::memory_order_relaxed), message.guest_frame)' not in lv:
        bad.append('live.cpp per-frame BoolSeen read with the guest frame')
    if 'rdr::g_stencil_dead.store(p.stencil_dead' not in lv or 'rdr::g_stencil_dead.store(render_globals.stencil_dead' not in lv:
        bad.append('live.cpp stores the global (set_globals and per frame)')
    r = fs['src/renderer/renderer.cpp']
    for needle, what in (('sd_on = g_stencil_dead.load(std::memory_order_relaxed)', 'Compile reads the switch every frame'),
                         ('if ((clear.flags & kClearStencil) && !sd_on) plan.stencil_used = true;', '(b) an in-pass clear is not a use'),
                         ('if (sd_on && plan.stencil_load == VK_ATTACHMENT_LOAD_OP_CLEAR)', '(a) the absorbed clear'),
                         ('!plan.stencil_used && !plan.stencil_after', '(a) only when no draw of the pass and nothing later reads'),
                         ('if (sd_on && (g_stencil_dead_plant.load(std::memory_order_relaxed) == 2 || !keep))', '(c) depth copies'),
                         ('stencil_needed[copy.source.image] = 1;', '(c) a kept copy carries the stencil need back to its source'),
                         ('if (pass.strip_stencil_clear && (clear.flags & kClearStencil))', '(b) the in-pass clear command drops its aspect'),
                         ('g.depth_only ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT)', 'RegionOf names the depth aspect only')):
        if needle not in r:
            bad.append('renderer.cpp: ' + what)
    if '"--stencil-dead"' not in fs['src/fview/main.cpp'] or '"--stencil-dead"' not in fs['src/renderer/player/rplay.cpp']:
        bad.append('fview / rplay flags')
    return bad


def wiring(root):
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'split'))
    from read_unit import read_path  # a split file is read with its parts expanded (tools/split/read_unit.py)
    fs = {}
    for f in FILES:
        fs[f] = read_path(os.path.join(root, f))
    bad = lint(fs)
    line(not bad, 'wiring: every link of the switch is in the source%s' % ('' if not bad else ' - MISSING: ' + '; '.join(bad)))
    plants = (('src/renderer_adapter/rdr_switches.h', 'X(StencilDead, "wwe2k14_rdr_stencil_dead", PerFrame, 0)', 'X(StencilDead, "wwe2k14_rdr_stencil_dead", StartOnly, 1)'),
              ('src/renderer_adapter/reader.h', 'REXCVAR_DEFINE_BOOL(wwe2k14_rdr_stencil_dead, false,', 'REXCVAR_DEFINE_BOOL(wwe2k14_rdr_stencil_dead, true,'),
              ('src/renderer_adapter/stage_split.h', 'stencil_dead = false', 'stencil_dead_x = false'),
              ('src/renderer_adapter/live.cpp', 'rdr::g_stencil_dead.store(render_globals.stencil_dead', 'rdr::g_stencil_dead.store(false'),
              ('src/renderer_adapter/live.cpp', 'rdr_sw::BoolSeen(rdr_sw::kStencilDead, rdr::g_stencil_dead.load(std::memory_order_relaxed), message.guest_frame)', 'rdr::g_stencil_dead.load(std::memory_order_relaxed)'),
              ('src/renderer/renderer.cpp', 'sd_on = g_stencil_dead.load(std::memory_order_relaxed)', 'sd_on = false'),
              ('src/renderer/renderer.cpp', '!plan.stencil_used && !plan.stencil_after', '!plan.stencil_used'),
              ('src/renderer/renderer.cpp', 'stencil_needed[copy.source.image] = 1;', ';'),
              ('src/renderer/renderer.cpp', 'if (pass.strip_stencil_clear && (clear.flags & kClearStencil))', 'if (false)'),
              ('src/fview/main.cpp', '"--stencil-dead"', '"--stencil-dead-x"'))
    for f, a, b in plants:
        if a not in fs[f]:
            line(False, 'wiring plant %s: DID NOT APPLY (the text %r is not in the file)' % (os.path.basename(f), a[:50]))
            continue
        copy = dict(fs)
        copy[f] = fs[f].replace(a, b, 1)
        if copy[f] == fs[f]:
            line(False, 'wiring plant DID NOT APPLY: %s' % a[:50])
            continue
        got = lint(copy)
        line(bool(got), 'wiring plant (%s: %s) is caught%s' % (os.path.basename(f), a[:46], '' if got else ' - NOT CAUGHT'))


# ---- the runs -----------------------------------------------------------------------------------------------------------------------------------------------------------
class Ctx:
    def __init__(self, root, frames):
        self.root = root
        self.main = C.main_checkout(root)
        self.frames = frames
        self.work = os.path.join(root, 'out', 'r24c')
        os.makedirs(self.work, exist_ok=True)
        self.exe = os.path.join(root, 'out', 'renderer', 'dev', 'fview.exe')
        self.game = os.path.join(self.main, 'game_data', 'extracted')

    def run(self, cap, tag, extra, frames=None):
        return C.run_fview(self.exe, os.path.join(self.main, C.CAPTURES[cap]), self.game, self.work, '%s.%s' % (cap, tag), C.BUNDLE + ['--depth-census'] + extra, frames=frames or self.frames)


STENCIL_CAPTURE = {'wm30'}   # the captures whose stream really uses stencil (volumes)


def capture_proof(cx, cap):
    off = cx.run(cap, 'off', [])
    on = cx.run(cap, 'on', ['--stencil-dead', '1'])
    c = census_of(off)
    if cap in STENCIL_CAPTURE:
        line(c is not None and c['used'] >= 1, '%s census: the stencil capture: stencil really used on %s depth images (%s)' % (cap, c and c['used'], c))
    else:
        line(c is not None and c['images'] > 0 and c['used'] == 0 and c['nonzero'] == 0, '%s census: no stencil use (%s)' % (cap, c))
    line(off.clean, '%s off arm: exit %d, validation errors %d' % (cap, off.rc, off.valid))
    line(on.clean, '%s on arm: exit %d, validation errors %d' % (cap, on.rc, on.valid))
    n = len([k for k in on.hashes if k != 'format'])
    line(n >= 50 and n == len([k for k in off.hashes if k != 'format']), '%s: %d frames hashed in each arm' % (cap, n))
    d = on.differing(off)
    line(d == 0, '%s: presented-surface hashes equal off vs on on every frame (%d differ)' % (cap, d))
    onp = cx.run(cap, 'onp', ['--stencil-dead', '1', '--stencil-dead-poison'])
    line(onp.clean, '%s on+poison arm: exit %d, validation errors %d' % (cap, onp.rc, onp.valid))
    d = onp.differing(off)
    line(d == 0, '%s: ON + POISON (a 4090 keeps stale stencil bytes, so the poison makes every removed write garbage) equals OFF on every frame (%d differ)' % (cap, d))
    lo, lp = lever_of(off), lever_of(onp)
    line(lo is not None and lp is not None and lo['oracle_bad'] == 0 and lp['oracle_bad'] == 0 and lp['poison'] == 1,
         '%s forward oracle: stencil-testing passes %s (off) / %s (on+poison), seeing an undefined stencil: %s / %s' % (cap, lo and lo['oracle_tests'], lp and lp['oracle_tests'], lo and lo['oracle_bad'], lp and lp['oracle_bad']))
    lv = lever_of(on)
    took = lv is not None and lv['mode'] == 'on' and lv['frames'] > 0 and (lv['abs_dropped'] + lv['in_stripped'] + lv['copy_depth']) > 0
    line(took, '%s on arm: the lever took (%s)' % (cap, lv))
    return off, on


def plants(cx):
    cap = 'wm30'
    off = cx.run(cap, 'p_off', [], frames=300)
    ref = cx.run(cap, 'p_on', ['--stencil-dead', '1'], frames=300)
    line(off.clean and ref.clean and ref.differing(off) == 0, 'plants: the reference (wm30, 300 frames) off vs on equal, validation 0')
    pp = cx.run(cap, 'pp1', ['--stencil-dead', '1', '--stencil-dead-poison', '--stencil-dead-plant', '1'], frames=300)
    lpp = lever_of(pp)
    line(pp.differing(off) > 0 and lpp is not None and lpp['oracle_bad'] > 0,
         'plant 1 + poison: the picture differs on %d frames AND the oracle sees %s stencil-testing passes with an undefined stencil (it catches what hash equality on a stale-bytes GPU cannot)' % (pp.differing(off), lpp and lpp['oracle_bad']))
    for n, what in ((1, 'an absorbed stencil clear dropped before a stencil test'), (3, 'an in-pass stencil clear stripped although a later pass reads it'), (2, 'every depth copy depth-only')):
        p = cx.run(cap, 'p%d' % n, ['--stencil-dead', '1', '--stencil-dead-plant', str(n)], frames=300)
        d = p.differing(off)
        lv = lever_of(ref)
        if n == 2:   # a copy that drops a live stencil: visible only when a later pass tests the copy's destination
            print('INFO  plant 2 (%s): %d frames differ, validation %d; the reference carried a stencil through %s depth copies' % (what, d, p.valid, lv and lv['copy_whole']))
            if lv and lv['copy_whole'] > 0:
                line(d > 0, 'plant 2 is caught by the picture (the capture has %d copies that carry a live stencil)' % lv['copy_whole'])
        elif n == 3:  # an in-pass clear that something reads: exists only when the stream clears stencil inside a pass
            print('INFO  plant 3 (%s): %d frames differ, validation %d; in-pass stencil clears in the reference: %s kept' % (what, d, p.valid, lv and lv['in_kept']))
            if lv and lv['in_kept'] > 0:
                line(d > 0, 'plant 3 is caught by the picture (%d in-pass clears are read later)' % lv['in_kept'])
        else:
            line(d > 0, 'plant %d (%s) is caught: %d frames differ' % (n, what, d))


def selftest(cx):
    rplay = os.path.join(cx.root, 'out', 'renderer', 'dev', 'rplay.exe')
    arms = (('off', [], True), ('on', ['--stencil-dead', '1'], True), ('on+poison', ['--stencil-dead', '1', '--stencil-dead-poison'], True),
            ('plant 2 (every depth copy depth-only) +poison', ['--stencil-dead', '1', '--stencil-dead-poison', '--stencil-dead-plant', '2'], False),
            ('plant 4 (a kept copy does not mark its source needed) +poison', ['--stencil-dead', '1', '--stencil-dead-poison', '--stencil-dead-plant', '4'], False),
            ('plant 3 (an in-pass clear stripped though read later) +poison', ['--stencil-dead', '1', '--stencil-dead-poison', '--stencil-dead-plant', '3'], False))
    for name, extra, want_pass in arms:
        r = subprocess.run([rplay, '--stencil-selftest', '--validation', '1'] + extra, capture_output=True, text=True, timeout=600, errors='replace')
        text = (r.stdout or '') + (r.stderr or '')
        verdict = re.search(r'\[rplay\]\[stencil_selftest\] (PASS|FAIL)', text)
        bad = [l for l in text.splitlines() if l.startswith('FAIL')]
        ok = verdict is not None and (verdict.group(1) == 'PASS') == want_pass and (r.returncode == 0) == want_pass
        line(ok, 'synthetic stencil scenes (Full / DisplayOnly / PersistentOnly), %s: %s%s' % (name, verdict.group(1) if verdict else 'NO VERDICT', '' if want_pass else ' (a plant: must fail; failed: %s)' % '; '.join(b[6:60] for b in bad[:2])))


def arms(cx):
    rplay = os.path.join(cx.root, 'out', 'renderer', 'dev', 'rplay.exe')
    cap = os.path.join(cx.main, 'out', 'captures', 'brock_s15')
    r = subprocess.run([rplay, '--capture', cap, '--frames', '40', '--game', cx.game, '--gpu-log', os.path.join(cx.work, 'arms_gpu.csv'), '--gpu-stamps', '1', '--gpu-pass-stamps', '1',
                        '--arms', 'off=7:0|on=7:1', '--stencil-dead-plant', '5'], capture_output=True, text=True, timeout=900, errors='replace')
    text = (r.stdout or '') + (r.stderr or '')
    rows = re.findall(r'\[rdr\]\[gpu\]\[passes\] (\w+ \w+ #\d+ image \d+ \d+x\d+ fmt \d+): (\d+) us mean .*?\[stencil_dead (ON|OFF): (\d+) frames\]', text)
    by = {}
    for key, mean, arm, n in rows:
        by.setdefault(key, {})[arm] = int(mean)
    both = {k: v for k, v in by.items() if 'ON' in v and 'OFF' in v}
    one_armed = {k: v for k, v in by.items() if not ('ON' in v and 'OFF' in v)}
    line(r.returncode == 0 and len(both) >= 3, 'in-run flip (rplay arms off=7:0|on=7:1): the per-step GPU table has an ON column and an OFF column (%d steps in both, %d printed in one arm only)' % (len(both), len(one_armed)))
    # the step with the largest ON - OFF gap (a one-armed step counts its printed arm against 0: reported, not dropped)
    gap = lambda v: v.get('ON', 0) - v.get('OFF', 0)
    top = max(by.items(), key=lambda kv: abs(gap(kv[1])), default=None)
    rest_ok = top is not None and all(abs(gap(v)) < 400 for k, v in by.items() if k != top[0])
    line(top is not None and gap(top[1]) >= 600 and rest_ok,
         'plant 5 (+1 ms on one step of every ON frame): the largest ON - OFF step is %s %s us%s, every other step differs by less than 400 us (an average of both arms would have hidden it)' % (top and top[0], top and top[1], ' (printed in one arm only)' if top and top[0] in one_armed else ''))


def valid3(cx):
    r3 = subprocess.run([cx.exe, '--case', os.path.join(cx.main, C.CAPTURES['wm30']), '--replay', '--frames', '120', '--validation', '3', '--game', cx.game] + C.BUNDLE + ['--stencil-dead', '1'],
                        capture_output=True, text=True, timeout=1700, errors='replace')
    text = (r3.stdout or '') + (r3.stderr or '')
    v = re.findall(r'validation errors (\d+)', text)
    line(r3.returncode == 0 and bool(v) and int(v[-1]) == 0, 'wm30, on, --validation 3 (synchronization + best practices): exit %d, errors %s' % (r3.returncode, v[-1] if v else '?'))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=C.ROOT)
    ap.add_argument('--only', default='')
    ap.add_argument('--frames', type=int, default=1000)
    a = ap.parse_args()
    only = set(x for x in a.only.split(',') if x)
    cx = Ctx(a.root, a.frames)
    want = lambda n: not only or n in only
    if want('unit'):
        unit(a.root)
    if want('wiring'):
        wiring(a.root)
    for cap in ('bryan', 'brock', 'match', 'wm30'):
        if want(cap):
            capture_proof(cx, cap)
    if want('plants'):
        plants(cx)
    if want('valid3'):
        valid3(cx)
    if want('selftest'):
        selftest(cx)
    if want('arms'):
        arms(cx)
    print('r24c: %d failed' % fails)
    sys.exit(1 if fails else 0)


if __name__ == '__main__':
    main()
