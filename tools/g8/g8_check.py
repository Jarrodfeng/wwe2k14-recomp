#!/usr/bin/env python3
"""S102 lane G8: the full-size alias rule (wwe2k14_rdr_alias_fullsize, src/renderer_adapter/copy_alias.h, docs/renderer/ALIAS_FULLSIZE.md). Offline, 0 game runs, 0 Surface runs.

  py -I tools\\g8\\g8_check.py [--root DIR] [--only unit|wiring|replay] [--capture bryan|match|both]

unit    out\\tools\\alias_fullsize_test.exe: the rule on synthetic 1280x720 frames; every planted fault (src_dirty, read_first, ignore_readback, format) must FAIL it.
wiring  the switch is in the S0-4 table (PerFrame, default 0), defined as a cvar, read per frame by live.cpp, carried to the binder, ended before Map() at a kept copy;
        each link removed in a text copy must fail the lint.
replay  fview headless on the Bryan entrance and the gate's match capture (presented-surface hashes, every frame):
          A = switch off; B = --alias-fullsize 2 (and 1 on Bryan): 0 frames differ, validation 0, violations 0 (the stale-read ledger), copies dropped a frame > 0,
          read-backs of aliased destinations 0, elided although read back 0; the players' combinations (shadows ours + 256 + half rate + copy alias classes 11; post ours) stay equal.
        rplay counts on Bryan (M1 counters): copies and barriers fall by exactly the census' elided a frame.
        Planted twins, each must be CAUGHT: src_dirty (a source rewritten before the read: the replay stops on a renderer [PROBLEM] or the pictures change); fs_no_clear (a kept copy
        treated as elided: the renderer's copies fall by MORE than the census says); fs_no_reader on the match (a copy nobody reads this frame is elided: the stale-read ledger fires);
        a planted CPU read-back of the aliased destinations: refused from then on (refused > 0, elided although read back 0, pictures equal), counted (read-backs of aliased destinations > 0),
        and its twin ignore_readback is caught by the census ('elided although read back' > 0)."""
import argparse
import json
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
BRYAN_COUNTS_DIR = 'out/s101/lane92/case_20261112_122719_969_0_pc_run_exit/capture'
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


FS = re.compile(r'full-size rule \(level (\d+)\): copies (\d+), admitted (\d+), elided (\d+) \(([\d.]+) MPix; strict alone (\d+), segments (\d+), depth (\d+), present (\d+)\), '
                r'refused for a CPU read-back (\d+), elided although read back (\d+) \(must be 0\), read-backs of aliased destinations (\d+)')


class Arm:
    def __init__(self, exe, zip_path, game, work, name, extra, allow_stop=False):
        hashes = os.path.join(work, name + '.csv')
        if os.path.exists(hashes):
            os.remove(hashes)
        cmd = [exe, '--case', zip_path, '--replay', '--frames', '1000', '--validation', '1', '--crowd-ours', '--crowd-cards', '--game', game, '--hash-frames', hashes] + extra
        for _ in range(12):
            p = subprocess.run(cmd, capture_output=True, text=True, timeout=1500, errors='replace')
            self.text = (p.stdout or '') + (p.stderr or '')
            if 'Not enough graphics memory for the frame ring' not in self.text:
                break
            time.sleep(20)
        with open(os.path.join(work, name + '.log'), 'w', encoding='utf-8') as f:
            f.write(self.text)
        self.rc = p.returncode   # S102 main (fview lint): 0 clean, 4 = validation errors (counted below); anything else is a crash
        if self.rc not in (0, 4) and not allow_stop:
            print(self.text[-2500:])
            raise RuntimeError('arm %s: fview exit %s (a crash, not a validation count)' % (name, self.rc))
        self.stopped = not os.path.isfile(hashes)
        if self.stopped and not allow_stop:
            print(self.text[-2500:])
            raise RuntimeError('arm %s wrote no hashes' % name)
        self.hashes = {} if self.stopped else read_hashes(hashes)
        v = re.findall(r'validation errors (\d+)', self.text)
        self.valid = int(v[-1]) if v else -1
        m = re.search(r'frames planned (\d+) \(skipped (\d+)\), violations (\d+)', self.text)
        self.frames, self.viol = (int(m.group(1)), int(m.group(3))) if m else (0, 0)
        m = FS.search(self.text)
        keys = ('level', 'copies', 'admitted', 'elided', 'mpix', 'strict', 'segments', 'depth', 'present', 'refused', 'elided_readback', 'readback_hits')
        self.fs = dict(zip(keys, [float(x) if '.' in x else int(x) for x in m.groups()])) if m else dict.fromkeys(keys, 0)
        self.per_frame = self.fs['elided'] / max(1, self.frames)
        m = re.search(r'lasting probe: (\d+) probes, (\d+) last pictures differ', self.text)
        self.probes, self.probe_bad = (int(m.group(1)), int(m.group(2))) if m else (0, 0)

    def differing(self, other):
        return sum(1 for k in self.hashes if k != 'format' and self.hashes.get(k) != other.hashes.get(k))


def rplay_counts(rplay, capdir, work, name, extra):
    out = os.path.join(work, name + '.json')
    p = subprocess.run([rplay, '--capture', capdir, '--frames', '100', '--counts', out] + extra, capture_output=True, text=True, timeout=900, errors='replace')
    if p.returncode != 0:
        raise RuntimeError('rplay %s exit %d: %s' % (name, p.returncode, (p.stdout + p.stderr)[-800:]))
    copies, barriers, n = 0, 0, 0
    with open(out, encoding='utf-8') as f:
        for ln in f:
            ln = ln.strip().rstrip(',')
            if ln.startswith('{"frame"'):
                d = json.loads(ln)
                if d['frame'] >= 10:   # (the first frames carry the start-up uploads)
                    copies += d['total']['copies']
                    barriers += d['total']['barriers']
                    n += 1
    return copies / n, barriers / n, p.stdout + p.stderr


# ---- unit -----------------------------------------------------------------------------------------------------------------------------------------------------------
def unit(root):
    r = subprocess.run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'g8', 'build_alias_fullsize_test.ps1')], capture_output=True, text=True, timeout=600)
    exe = os.path.join(root, 'out', 'tools', 'alias_fullsize_test.exe')
    line(r.returncode == 0 and os.path.isfile(exe), 'unit test builds')
    if not os.path.isfile(exe):
        return
    r = subprocess.run([exe], capture_output=True, text=True, timeout=120)
    oks = sum(1 for l in r.stdout.splitlines() if l.startswith('ok'))
    line(r.returncode == 0 and 'all passed' in r.stdout, 'unit test: the shipped rule passes its %d expectations' % oks)
    for plant in ('src_dirty', 'read_first', 'ignore_readback', 'format', 'no_draw_writes', 'fs_no_exclusion', 'empty'):
        r = subprocess.run([exe, '--plant', plant], capture_output=True, text=True, timeout=120)
        first = next((l for l in r.stdout.splitlines() if l.startswith('FAIL')), '')
        line(r.returncode == 1, 'unit plant %s is caught: %s' % (plant, first[:100] if first else 'NOT CAUGHT'))


# ---- wiring ---------------------------------------------------------------------------------------------------------------------------------------------------------
def wiring(root):
    def rd(p):
        with open(os.path.join(root, p), encoding='utf-8', errors='replace') as f:
            return f.read()
    files = {k: rd(k) for k in ('src/renderer_adapter/rdr_switches.h', 'src/renderer_adapter/reader.h', 'src/renderer_adapter/live.cpp', 'src/renderer_adapter/binder.h',
                                'src/renderer_adapter/copy_alias.h', 'src/fview/main.cpp', 'src/renderer/player/rplay.cpp')}

    def lint(fs):
        bad = []
        t = re.search(r'X\(AliasFullsize, "wwe2k14_rdr_alias_fullsize", (\w+), (\d+)\)', fs['src/renderer_adapter/rdr_switches.h'])
        if not t or t.group(1) != 'PerFrame' or t.group(2) != '0':
            bad.append('table line (PerFrame, default 0)')
        if 'REXCVAR_DEFINE_INT32(wwe2k14_rdr_alias_fullsize, 0,' not in fs['src/renderer_adapter/reader.h']:
            bad.append('cvar definition (default 0)')
        if 'copy_alias::g_fullsize = copy_alias::ClampLevel(int(rdr_sw::IntSeen(rdr_sw::kAliasFullsize' not in fs['src/renderer_adapter/live.cpp']:
            bad.append('live.cpp reads the switch every frame')
        b = fs['src/renderer_adapter/binder.h']
        if 'facts.fullsize = (g_dlss || g_velocity_census) ? 0 : copy_alias::ClampLevel(copy_alias::g_fullsize)' not in b:
            bad.append('binder carries the level into the planner (and refuses it under DLSS)')
        i, j = b.find('EndAliasOf(copy.dest.image)'), b.find('copy.dest.image = Map(copy.dest.image)')
        if i < 0 or j < 0 or i > j:
            bad.append('binder ends the alias BEFORE Map() maps a kept copy\'s destination')
        if 'copy_alias::g_mode <= 0 && copy_alias::g_fullsize <= 0' not in b:
            bad.append('binder plans when only the full-size switch is on')
        if 'if ((!persist_only_bind && !frame_part_open) ||' not in b:
            bad.append('a persist-only / part bind must not replace the last picture (F1)')
        if 'present_ok = !stage_frame_on_' not in b:
            bad.append('the Present alias is off for stage-view frames')
        if '--alias-fullsize' not in fs['src/fview/main.cpp'] or '--alias-fullsize' not in fs['src/renderer/player/rplay.cpp']:
            bad.append('fview / rplay flags')
        c = fs['src/renderer_adapter/copy_alias.h']
        if 'inline int g_fullsize = 0;' not in c:
            bad.append('copy_alias.h default 0')
        if 'g_fault != Fault::IgnoreReadback && WasReadBack(D)' not in c:
            bad.append('read-back refusal')
        if 'if (g_fs_aliased_ids.count(image)) ++g_fs_readback_hits;' not in c:
            bad.append('read-back of an aliased destination is counted')
        return bad

    bad = lint(files)
    line(not bad, 'wiring: the switch is in the table, defined, read per frame, carried to the planner, alias ended before Map() ' + ('' if not bad else 'MISSING: ' + '; '.join(bad)))
    twins = [('src/renderer_adapter/rdr_switches.h', 'PerFrame, 0)  /* S102 G8', 'StartOnly, 0)  /* S102 G8'),
             ('src/renderer_adapter/reader.h', 'REXCVAR_DEFINE_INT32(wwe2k14_rdr_alias_fullsize, 0,', 'REXCVAR_DEFINE_INT32(wwe2k14_rdr_alias_fullsize, 1,'),
             ('src/renderer_adapter/live.cpp', 'rdr_sw::kAliasFullsize', 'rdr_sw::kCardCut'),
             ('src/renderer_adapter/binder.h', 'facts.fullsize = (g_dlss || g_velocity_census) ? 0 : copy_alias::ClampLevel(copy_alias::g_fullsize)', 'facts.fullsize = 0'),
             ('src/renderer_adapter/binder.h', 'if ((!persist_only_bind && !frame_part_open) ||', 'if (true ||'),
             ('src/renderer_adapter/live.cpp', 'copy_alias::g_fullsize = copy_alias::ClampLevel(', 'copy_alias::g_fullsize = ('),
             ('src/renderer_adapter/binder.h', 'if (alias_on_ && g_own_plane_fault != OwnPlaneFault::NoAliasEnd && EndAliasOf(copy.dest.image)) return;', 'if (alias_on_) {}'),  # S102 main: P3 moved the alias end into IssueCopy
             ('src/renderer_adapter/binder.h', 'copy_alias::g_mode <= 0 && copy_alias::g_fullsize <= 0', 'copy_alias::g_mode <= 0'),
             ('src/renderer_adapter/copy_alias.h', 'g_fault != Fault::IgnoreReadback && WasReadBack(D)', 'WasReadBack(D) && false')]
    for path, old, new in twins:
        if old not in files[path]:
            line(False, 'wiring twin: the text %r is not in %s (the lint is stale)' % (old[:40], path))
            continue
        twin = dict(files)
        twin[path] = files[path].replace(old, new, 1)
        line(bool(lint(twin)), 'wiring twin (%s: %r -> %r) is caught' % (os.path.basename(path), old[:36], new[:30]))
    # the order twin: the alias ended AFTER Map
    twin = dict(files)
    b = files['src/renderer_adapter/binder.h']
    a1, a2 = 'if (alias_on_ && g_own_plane_fault != OwnPlaneFault::NoAliasEnd && EndAliasOf(copy.dest.image)) return;', 'copy.dest.image = Map(copy.dest.image);'  # S102 main: IssueCopy (P3)
    if a1 in b and a2 in b:
        twin['src/renderer_adapter/binder.h'] = b.replace(a1, 'XX', 1).replace(a2, a2 + ' ' + a1, 1)
        line(bool(lint(twin)), 'wiring twin (the alias ended after Map()) is caught')


# ---- replay ---------------------------------------------------------------------------------------------------------------------------------------------------------
def replay(root, which):
    main_co = main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    sys.path.insert(0, os.path.join(root, 'tools', 'fview'))
    import fview_exe as fe
    exe = fe.find_or_exit(root=root, kind='dev')
    work = os.path.join(root, 'out', 'g8_check')
    os.makedirs(work, exist_ok=True)
    for n in (['bryan', 'match'] if which == 'both' else [which]):
        z = os.path.join(main_co, CAPTURES[n])
        if not os.path.exists(z):
            line(False, 'input missing: ' + z)
            continue
        A = Arm(exe, z, game, work, n + '_A', [])
        want3 = {'bryan': 4.5, 'match': 2.5}[n]
        B = Arm(exe, z, game, work, n + '_B3', ['--alias-fullsize', '3'])
        d = B.differing(A)
        f = B.fs
        line(d == 0 and len(B.hashes) == len(A.hashes) > 0 and A.valid == 0 and B.valid == 0 and B.viol == 0 and B.per_frame >= want3 and f['readback_hits'] == 0 and f['elided_readback'] == 0
             and f['depth'] > 0 and f['present'] > 0,
             'level 3 %s: %d frames, %d differ, validation %d/%d, stale-read ledger (violations) %d, copies dropped %.2f a frame (%.1f MPix a frame; segments %d, depth %d, present %d), read-backs of aliased dests %d' %
             (n, len(A.hashes) - 1, d, A.valid, B.valid, B.viol, B.per_frame, f['mpix'] / max(1, B.frames), f['segments'], f['depth'], f['present'], f['readback_hits']))
        # levels 1 and 2 never touch the Present: the capture's own Present image (Map(surface), --hash-mapped) must hold the picture too (review item 6)
        for lv, minpf in ((1, {'bryan': 3.5, 'match': 0.0}[n]), (2, {'bryan': 3.5, 'match': 1.5}[n])):
            L = Arm(exe, z, game, work, '%s_L%d' % (n, lv), ['--alias-fullsize', str(lv)])
            M = Arm(exe, z, game, work, '%s_L%dm' % (n, lv), ['--alias-fullsize', str(lv), '--hash-mapped'])
            line(L.differing(A) == 0 and M.differing(A) == 0 and L.valid == 0 and M.valid == 0 and L.viol == 0 and L.per_frame >= minpf and L.fs['present'] == 0 and len(M.hashes) == len(A.hashes) > 0,
                 'level %d %s: presented picture %d differ, Map(surface) picture %d differ, validation %d, violations %d, copies dropped %.2f a frame, Present untouched (%d)' %
                 (lv, n, L.differing(A), M.differing(A), L.valid, L.viol, L.per_frame, L.fs['present']))
        # the switch off by default: the same replay with no flag writes no full-size line and elides nothing the strict rule keeps
        line(A.fs['elided'] == 0 and 'full-size rule' not in A.text, 'switch off: nothing elided, no full-size line (%s)' % n)
        # F1: a skipped frame that runs lasting work must not replace the last picture (level 3: the Present is the one that reads an aliased image)
        Pr = Arm(exe, z, game, work, n + '_probe', ['--alias-fullsize', '3', '--lasting-probe'])
        line(Pr.probes > 0 and Pr.probe_bad == 0, 'last picture after a persist-only lasting bind (level 3, %s): %d probes, %d differ from the drawn frame' % (n, Pr.probes, Pr.probe_bad))
        Pg = Arm(exe, z, game, work, n + '_probe_twin', ['--alias-fullsize', '3', '--lasting-probe', '--copy-alias-fault', 'no_present_guard'])
        line(Pg.probes > 0 and Pg.probe_bad > 0, 'twin no_present_guard (the guard removed) is caught: %d of %d last pictures differ' % (Pg.probe_bad, Pg.probes))
        if n == 'bryan':
            # the combinations players run
            E = Arm(exe, z, game, work, n + '_E', ['--shadows-ours', '--shadow-map-size', '256', '--shadow-half'])
            F = Arm(exe, z, game, work, n + '_F', ['--shadows-ours', '--shadow-map-size', '256', '--shadow-half', '--alias-fullsize', '3', '--copy-alias', '2', '--copy-alias-classes', '11'])
            line(F.differing(E) == 0 and E.valid == 0 and F.valid == 0 and F.viol == 0 and F.per_frame >= 1,
                 'LOW combination %s (shadows ours, 256, half rate, copy alias classes 11) with the full-size rule on: %d differ, validation %d/%d, violations %d, full-size drops %.2f a frame' % (n, F.differing(E), E.valid, F.valid, F.viol, F.per_frame))
            G = Arm(exe, z, game, work, n + '_G', ['--post-ours'])
            H = Arm(exe, z, game, work, n + '_H', ['--post-ours', '--alias-fullsize', '3'])
            line(H.differing(G) == 0 and G.valid == 0 and H.valid == 0 and H.viol == 0 and H.per_frame >= 1,
                 'post ours %s with the full-size rule on: %d differ, validation %d/%d, violations %d, full-size drops %.2f a frame' % (n, H.differing(G), G.valid, H.valid, H.viol, H.per_frame))
            # planted twins
            P = Arm(exe, z, game, work, n + '_P1', ['--alias-fullsize', '3', '--copy-alias-fault', 'src_dirty'], allow_stop=True)
            stopped = P.stopped and '[rdr][PROBLEM]' in P.text
            line(stopped or (not P.stopped and P.differing(A) > 0),
                 'twin src_dirty (an aliased destination whose source is rewritten before the read) is caught: %s' % ('the replay stopped on a renderer [PROBLEM]' if stopped else '%d frames change' % P.differing(A)))
            R = Arm(exe, z, game, work, n + '_R', ['--alias-fullsize', '3', '--copy-alias-readback', '30:2,5,1'])
            line(R.differing(A) == 0 and R.fs['refused'] > 0 and R.fs['elided_readback'] == 0 and R.fs['readback_hits'] >= 1 and R.per_frame < B.per_frame / 2,
                 'a planted CPU read-back of the aliased destinations at frame 30: counted (%d), refused from then on (%d copies kept for it), none elided although read back (%d), pictures equal (%d differ), %.2f a frame (was %.2f)' %
                 (R.fs['readback_hits'], R.fs['refused'], R.fs['elided_readback'], R.differing(A), R.per_frame, B.per_frame))
            R2 = Arm(exe, z, game, work, n + '_R2', ['--alias-fullsize', '3', '--copy-alias-readback', '30:2,5,1', '--copy-alias-fault', 'ignore_readback'])
            line(R2.fs['elided_readback'] > 0 and R2.fs['refused'] == 0,
                 'twin ignore_readback (a read-back destination stays aliased) is caught by the census: elided although read back %d' % R2.fs['elided_readback'])
            # renderer counts: copies and barriers fall by exactly the census' drops a frame; the swallowed-copy plant falls by more
            rplay = os.path.join(root, 'out', 'renderer', 'dev', 'rplay.exe')
            capdir = os.path.join(main_co, BRYAN_COUNTS_DIR)
            if os.path.isfile(rplay) and os.path.isdir(capdir):
                ca_, ba_, _ = rplay_counts(rplay, capdir, work, 'cnt_A', [])
                cb_, bb_, tb = rplay_counts(rplay, capdir, work, 'cnt_B', ['--alias-fullsize', '3'])
                m = FS.search(tb)
                census = (int(m.group(4)) / 100.0) if m else -1   # (rplay's census counts all planned frames: 100)
                line(abs((ca_ - cb_) - census) < 0.2 and abs((ba_ - bb_) - census) < 0.2 and census >= 4.5,
                     'renderer counts (rplay, 100 frames): copies %.2f -> %.2f a frame (-%.2f), barriers %.2f -> %.2f (-%.2f); census drops %.2f a frame' % (ca_, cb_, ca_ - cb_, ba_, bb_, ba_ - bb_, census))
                cp, _, tp = rplay_counts(rplay, capdir, work, 'cnt_P', ['--alias-fullsize', '3', '--copy-alias-fault', 'fs_no_clear'])
                line(abs((ca_ - cp) - census) > 0.5, 'twin fs_no_clear (a kept copy treated as elided) is caught by the counts: copies fall by %.2f a frame, the census says %.2f' % (ca_ - cp, census))
            else:
                line(False, 'rplay or its capture is missing: %s %s' % (rplay, capdir))
        else:
            P = Arm(exe, z, game, work, n + '_P3', ['--alias-fullsize', '3', '--copy-alias-fault', 'fs_no_reader'], allow_stop=True)
            stopped = P.stopped and '[rdr][PROBLEM]' in P.text
            line(stopped or P.viol > 0 or P.differing(A) > 0,
                 'twin fs_no_reader (a copy nobody reads this frame, read by the next frame before any write) is caught by the stale-read ledger: violations %d, %d frames change%s' %
                 (P.viol, P.differing(A), ', the replay stopped' if stopped else ''))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--only', default='')
    ap.add_argument('--capture', default='both')
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    if args.only in ('', 'unit'):
        unit(root)
    if args.only in ('', 'wiring'):
        wiring(root)
    if args.only in ('', 'replay'):
        replay(root, args.capture)
    print('G8: %s' % ('all passed' if not fails else '%d FAILED' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
