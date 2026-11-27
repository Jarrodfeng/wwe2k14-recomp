#!/usr/bin/env python3
"""S102 lane C3: the frame buffer's and the shadow maps' copies that exist only because the 360 had eDRAM (docs/renderer/COPIES_LEFT.md section 5). Offline, 0 game runs, 0 Surface runs.

  py -I tools\\c3\\c3_check.py [--root DIR] [--only wiring|core|default|low|batch|g11|plants|match|brock|counts]

Two switches, each PerFrame, default off:
  shadow_direct  wwe2k14_rdr_shadow_direct  (copy_alias.h `sd` rule, binder.h): with shadows_ours on, the whole copies of our shadow map into the game's map texture are not made; the readers sample our map.
  frame_direct   wwe2k14_rdr_frame_direct   (copy_alias.h: the full-size rule at level 3 under its own switch and census, fview / rplay --frame-direct): the scene resolve copies and the Present's copy go.
Every arm is fview headless (--validation 1) on a standard capture: the presented picture's hash of EVERY frame, the validation error count AND the exit code are read (never "the csv exists").
wiring   each link of both switches (table PerFrame default 0, cvar default false, live.cpp per-frame read, binder facts, census lines, fview / rplay flags); each removed in a text copy must fail the lint
         (a text twin that no longer applies fails as DID NOT APPLY).
core     bryan: the whole bundle off, + shadow direct, + frame direct, + both: byte-equal, validation 0, exit 0, taken counts read from the census lines.
default  bryan: the DEFAULT bundle the players run (shadows ours + casters + plane + crowd) with both on: frames taken (0 taken = nothing for players).
low      bryan: the default bundle + LOW levers (--shadow-map-size 256 --shadow-half --copy-alias 2 --copy-alias-classes 11 --alias-fullsize 1): equal; shadow direct declines under the half rate (counted).
batch    bryan: G12's copy batch ON in both arms. g11: G11's self blit + restore ON in both arms.
plants   twins that must fail: a reader still on the dropped copy's destination (sd / fd), the Present reading a stale image, the shadow readers re-pointed on a half-rate frame.
match / brock  the match capture and Check B / brock: equal, validation 0, exit 0.
counts   rplay --counts: copies / passes / barriers / pipeline binds a frame (G12's copy batch on in every arm): copies fall by what was taken, passes, barriers and binds do not rise."""
import argparse
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
CAP_ZIP = {
    'bryan': 'out/s101/runs/20261112_122700_s100_owner_bryan_entrance/case_20261112_122719_969_0_pc_run_exit.zip',
    'match': 'out/s101/runs/20261112_131008_s100_owner_match/case_20261112_131027_265_0_pc_run_exit.zip',
}
CAP_DIR = {'brock': 'out/lane86/brock/capture', 'bryan86': 'out/lane86/bryan/capture'}
BRYAN_COUNTS_DIR = 'out/s101/lane92/case_20261112_122719_969_0_pc_run_exit/capture'
BUNDLE = ['--crowd-ours', '--crowd-cards', '--arena-ours', '--wrestlers-ours', '--caw-ours', '--ring-ours', '--ui-ours', '--ui-quads', '--ui-text-ours', '--flash-ours', '--props-ours', '--post-ours',
          '--movie-ours', '--screens-ours', '--shadow-casters-ours', '--rim-ours', '--circles-ours', '--plane-shadow-ours', '--small-passes-ours', '--rim-chain-ours', '--particles-ours', '--shadows-ours']
DEFAULT3 = ['--crowd-ours', '--crowd-cards', '--shadows-ours', '--shadow-casters-ours', '--plane-shadow-ours']
LOW = ['--shadow-map-size', '256', '--shadow-half', '--copy-alias', '2', '--copy-alias-classes', '11', '--alias-fullsize', '1']
SD = ['--shadow-direct', '1']
FD = ['--frame-direct', '1']
FRAMES = 120
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


SDL = re.compile(r'\[rdr\]\[shadow_direct\] frames (\d+), taken (\d+) \(in (\d+) frames, ([\d.]+) MPix\), read-backs of aliased destinations (\d+), D3 frames left alone (\d+), declined (\d+) \(by reason:([^)]*)\)')
FDL = re.compile(r"\[rdr\]\[frame_direct\] effective level 3 \(G8 alias_fullsize (\d+)\), frames (\d+), taken (\d+) \(in (\d+) frames, ([\d.]+) MPix; the Present's (\d+); (\d+) more already taken by alias_fullsize\), read-backs of aliased destinations (\d+), declined (\d+) \(by reason:([^)]*)\)")


class Arm:
    """One fview replay of a standard capture. `cap` = ('zip', path) or ('dir', path)."""

    def __init__(self, exe, cap, game, work, name, extra, frames=FRAMES, allow_stop=False):
        hashes = os.path.join(work, name + '.csv')
        if os.path.exists(hashes):
            os.remove(hashes)
        kind, path = cap
        cmd = [exe, '--case' if kind == 'zip' else '--capture', path, '--replay', '--frames', str(frames), '--validation', '1', '--game', game, '--hash-frames', hashes] + extra
        for _ in range(12):
            p = subprocess.run(cmd, capture_output=True, text=True, timeout=1700, errors='replace')
            self.text = (p.stdout or '') + (p.stderr or '')
            if 'Not enough graphics memory for the frame ring' not in self.text:
                break
            time.sleep(20)
        self.rc = p.returncode
        with open(os.path.join(work, name + '.log'), 'w', encoding='utf-8') as f:
            f.write(self.text)
        self.stopped = not os.path.isfile(hashes)
        if self.stopped and not allow_stop:
            print(self.text[-2500:])
            raise RuntimeError('arm %s wrote no hashes' % name)
        self.hashes = {} if self.stopped else read_hashes(hashes)
        v = re.findall(r'validation errors (\d+)', self.text)
        self.valid = int(v[-1]) if v else -1
        m = re.search(r'frames planned (\d+) \(skipped (\d+)\), violations (\d+)', self.text)
        self.planned, self.viol = (int(m.group(1)), int(m.group(3))) if m else (0, 0)
        self.n = len([k for k in self.hashes if k != 'format'])
        m = re.search(r'lasting probe: (\d+) probes, (\d+) last pictures differ', self.text)
        self.probes, self.probe_bad = (int(m.group(1)), int(m.group(2))) if m else (0, 0)
        m = SDL.search(self.text)
        self.sd = dict(frames=int(m.group(1)), taken=int(m.group(2)), taken_frames=int(m.group(3)), mpix=float(m.group(4)), hits=int(m.group(5)), d3=int(m.group(6)), declined=int(m.group(7)), why=m.group(8).strip()) if m else dict(frames=0, taken=0, taken_frames=0, mpix=0.0, hits=0, d3=0, declined=0, why='')
        m = FDL.search(self.text)
        self.fd = dict(level=int(m.group(1)), frames=int(m.group(2)), taken=int(m.group(3)), taken_frames=int(m.group(4)), mpix=float(m.group(5)), present=int(m.group(6)), by_base=int(m.group(7)), hits=int(m.group(8)), declined=int(m.group(9)), why=m.group(10).strip()) if m else dict(level=0, frames=0, taken=0, taken_frames=0, mpix=0.0, present=0, by_base=0, hits=0, declined=0, why='')
        # the exit code AND the error count: fview writes its csv before it exits 4 on validation errors (G5 review)
        self.clean = self.rc == 0 and self.valid == 0

    def differing(self, other):
        return sum(1 for k in self.hashes if k != 'format' and self.hashes.get(k) != other.hashes.get(k))

    def per(self, d):
        return d['taken'] / max(1, self.n)


def setup(root, which):
    main_co = main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    sys.path.insert(0, os.path.join(root, 'tools', 'fview'))
    import fview_exe as fe
    exe = fe.find_or_exit(root=root, kind='dev')
    work = os.path.join(root, 'out', 'c3_check')
    os.makedirs(work, exist_ok=True)
    if which in CAP_ZIP:
        cap = ('zip', os.path.join(main_co, CAP_ZIP[which]))
    else:
        cap = ('dir', os.path.join(main_co, CAP_DIR[which]))
    if not os.path.exists(cap[1]):
        line(False, 'input missing: ' + cap[1])
        return None
    return exe, cap, game, work


def pair(exe, cap, game, work, tag, base_flags, new_flags, frames=FRAMES, want=None, label=''):
    """base arm and base + new_flags: equal pictures, clean, and (want) the taken counts a frame."""
    A = Arm(exe, cap, game, work, tag + '_A', base_flags, frames)
    B = Arm(exe, cap, game, work, tag + '_B', base_flags + new_flags, frames)
    d = B.differing(A)
    sd, fd = B.per(B.sd), B.per(B.fd)
    ok = A.clean and B.clean and d == 0 and len(A.hashes) == len(B.hashes) and A.n > 1 and B.viol == 0
    if want:
        ok = ok and sd >= want.get('sd', 0) and fd >= want.get('fd', 0)
    line(ok, '%s: %d frames, %d differ, validation %d / %d, exit %d / %d, violations %d | shadow direct %.2f a frame (%d declined: %s) | frame direct %.2f a frame (the Present %d, %d declined: %s)' %
         (label or tag, B.n, d, A.valid, B.valid, A.rc, B.rc, B.viol, sd, B.sd['declined'], B.sd['why'] or '-', fd, B.fd['present'], B.fd['declined'], B.fd['why'] or '-'))
    return A, B


# ---- wiring ---------------------------------------------------------------------------------------------------------------------------------------------------------
FILES = ('src/renderer_adapter/rdr_switches.h', 'src/renderer_adapter/reader.h', 'src/renderer_adapter/live.cpp', 'src/renderer_adapter/binder.h', 'src/renderer_adapter/copy_alias.h',
         'src/fview/main.cpp', 'src/renderer/player/rplay.cpp', 'src/fview/session.cpp')


def lint(fs):
    bad = []
    t = fs['src/renderer_adapter/rdr_switches.h']
    for name, cvar in (('ShadowDirect', 'wwe2k14_rdr_shadow_direct'), ('FrameDirect', 'wwe2k14_rdr_frame_direct')):
        m = re.search(r'X\(%s, "%s", (\w+), (\d+)\)' % (name, cvar), t)
        if not m or m.group(1) != 'PerFrame' or m.group(2) != '0':
            bad.append('table line %s (PerFrame, default 0)' % name)
    r = fs['src/renderer_adapter/reader.h']
    for cvar in ('wwe2k14_rdr_shadow_direct', 'wwe2k14_rdr_frame_direct'):
        if 'REXCVAR_DEFINE_BOOL(%s, false,' % cvar not in r:
            bad.append('cvar %s default false' % cvar)
    if "G8's full-size alias at level 3" not in r or 'level 3' not in t:
        bad.append('F6: the switch texts say "G8 full-size alias at level 3, own switch and census"')
    lv = fs['src/renderer_adapter/live.cpp']
    for a, b in (('g_shadow_direct', 'kShadowDirect'), ('g_frame_direct', 'kFrameDirect')):
        if 'copy_alias::%s = rdr_sw::BoolSeen(rdr_sw::%s,' % (a, b) not in lv:
            bad.append('live.cpp reads %s every frame' % b)
    b = fs['src/renderer_adapter/binder.h']
    if 'facts.shadow_direct = copy_alias::g_shadow_direct && shadows_active_ && !d3_blind;' not in b:
        bad.append('binder carries shadow direct into the planner (only with shadows ours active, never on a D3 marker frame)')
    if 'const bool d3_blind = d3_on_ || d3r_on_ || wr_on_ || d3c_on_;' not in b or 'if (d3_blind) facts.frame_direct = false' not in b:
        bad.append('F7: both switches off on a D3 marker frame')
    if 'facts.base_fullsize = ' not in b:
        bad.append('F5: the planner knows the player\'s own alias_fullsize')
    if 'FdSaveLastPicture(targets, builder);' not in b or 'builder.Copy(copy);   // allow:last_picture' not in b:
        bad.append('F3: a part / persist bind saves the last picture before it draws into it')
    if 'shadow_sets_.Empty() || shadow_sets_.Skips(id) || sd_blocked_dests_.count(id)' not in b or 'if (shadow_sets_.Skips(c.source.image)) sd_blocked_dests_.insert(c.dest.image);' not in b:
        bad.append('F1: the half rate blocks a destination a skipped source feeds, and everything until the maps are learned (shadow_blocked)')
    if 'turns_planner_.Tracks(id) || (plane_blank_enabled_ && plane_blank_planner_.IsChainImage(id));' not in b:
        bad.append('shadow_blocked keeps the turn / plane-blank terms')
    if 'facts.frame_direct = copy_alias::g_frame_direct && !(g_dlss || g_velocity_census);' not in b:
        bad.append('binder carries frame direct into the planner (never with DLSS / a velocity pass)')
    if 'if (facts.frame_direct && facts.fullsize < 3) facts.fullsize = 3;' not in b:
        bad.append('frame direct raises the full-size rule to level 3')
    if '&& !copy_alias::g_shadow_direct && !copy_alias::g_frame_direct) return;' not in b:
        bad.append('binder plans when only a direct switch is on')
    if 'Fault::SdNoAlias && op_index < alias_plan_.sd.size()' not in b or 'Fault::FdPresentStale && op_index < alias_plan_.fd.size()' not in b:
        bad.append('the planted faults act in IssueCopy')
    c = fs['src/renderer_adapter/copy_alias.h']
    if 'inline bool g_shadow_direct = false;' not in c or 'inline bool g_frame_direct = false;' not in c:
        bad.append('copy_alias.h defaults off')
    if 'if (g_fault != Fault::SdNoBlock && facts.shadow_blocked && (facts.shadow_blocked(S) || facts.shadow_blocked(D))) return kWhyExcluded;' not in c:
        bad.append('Judge: a half-rate / turn / plane-blank image is never aliased by shadow direct')
    if 'if (sd) relax = 2;' not in c:
        bad.append('Judge: shadow direct judges by the segment rule')
    if 'const bool sd = facts.shadow_direct && SdPair(facts, copy);' not in c or 'return a && b && (a == b || g_fault == Fault::SdSizeBlind);' not in c:
        bad.append('F10: shadow direct takes only a copy between two of OUR images of the SAME size')
    if 'if (present_used && g_fault != Fault::FdNoPresentTail' not in c:
        bad.append('F4: a Present-read destination checks writes to the source up to the end of the frame')
    if 'static_assert(sizeof(k) / sizeof(k[0]) == size_t(Fault::Count)' not in c:
        bad.append('F8: the fault table is size-checked')
    if 'NoteSdAliased(copy.dest.image);' not in c or 'if (g_sd_aliased_ids.count(image)) ++g_sd_readback_hits;' not in c:
        bad.append('F2: read-backs of shadow-direct destinations are counted')
    if '[rdr][shadow_direct] frames' not in c or '[rdr][frame_direct] effective level 3' not in c:
        bad.append('both census lines')
    if 'g_frame_direct) && !rdr_adapter::copy_alias::g_hash_mapped' not in fs['src/fview/session.cpp']:
        bad.append('fview hashes the presented image under frame direct')
    for path in ('src/fview/main.cpp', 'src/renderer/player/rplay.cpp'):
        for flag in ('--shadow-direct', '--frame-direct'):
            if '"%s"' % flag not in fs[path]:
                bad.append('%s flag %s' % (os.path.basename(path), flag))
    return bad


def wiring(root):
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'split'))
    from read_unit import read_path  # a split file is read with its parts expanded (tools/split/read_unit.py)
    files = {}
    for k in FILES:
        files[k] = read_path(os.path.join(root, k))
    bad = lint(files)
    line(not bad, 'wiring: both switches are in the table (PerFrame, default 0), defined (default false), read per frame, carried to the planner, with census lines and flags ' + ('' if not bad else 'MISSING: ' + '; '.join(bad)))
    twins = [('src/renderer_adapter/rdr_switches.h', 'X(ShadowDirect, "wwe2k14_rdr_shadow_direct", PerFrame, 0)', 'X(ShadowDirect, "wwe2k14_rdr_shadow_direct", StartOnly, 0)'),
             ('src/renderer_adapter/rdr_switches.h', 'X(FrameDirect, "wwe2k14_rdr_frame_direct", PerFrame, 0)', 'X(FrameDirect, "wwe2k14_rdr_frame_direct", PerFrame, 1)'),
             ('src/renderer_adapter/reader.h', 'REXCVAR_DEFINE_BOOL(wwe2k14_rdr_shadow_direct, false,', 'REXCVAR_DEFINE_BOOL(wwe2k14_rdr_shadow_direct, true,'),
             ('src/renderer_adapter/reader.h', 'REXCVAR_DEFINE_BOOL(wwe2k14_rdr_frame_direct, false,', 'REXCVAR_DEFINE_BOOL(wwe2k14_rdr_frame_direct, true,'),
             ('src/renderer_adapter/live.cpp', 'rdr_sw::BoolSeen(rdr_sw::kShadowDirect,', 'rdr_sw::BoolSeen(rdr_sw::kCardCut,'),
             ('src/renderer_adapter/live.cpp', 'rdr_sw::BoolSeen(rdr_sw::kFrameDirect,', 'rdr_sw::BoolSeen(rdr_sw::kCardCut,'),
             ('src/renderer_adapter/binder.h', 'facts.shadow_direct = copy_alias::g_shadow_direct && shadows_active_ && !d3_blind;', 'facts.shadow_direct = copy_alias::g_shadow_direct && shadows_active_;'),
             ('src/renderer_adapter/binder.h', 'shadow_sets_.Empty() || shadow_sets_.Skips(id) || sd_blocked_dests_.count(id)', 'false'),
             ('src/renderer_adapter/binder.h', 'if (shadow_sets_.Skips(c.source.image)) sd_blocked_dests_.insert(c.dest.image);', ''),
             ('src/renderer_adapter/binder.h', 'FdSaveLastPicture(targets, builder);   // S102 C3 F3', ''),
             ('src/renderer_adapter/binder.h', 'if (d3_blind) facts.frame_direct = false', 'if (false) facts.frame_direct = false'),
             ('src/renderer_adapter/copy_alias.h', 'if (present_used && g_fault != Fault::FdNoPresentTail', 'if (false && g_fault != Fault::FdNoPresentTail'),
             ('src/renderer_adapter/copy_alias.h', 'return a && b && (a == b || g_fault == Fault::SdSizeBlind);', 'return a && b;'),
             ('src/renderer_adapter/copy_alias.h', 'NoteSdAliased(copy.dest.image);', ''),
             ('src/renderer_adapter/reader.h', "G8's full-size alias at level 3", "the frame buffer's copies"),
             ('src/renderer_adapter/binder.h', 'facts.frame_direct = copy_alias::g_frame_direct && !(g_dlss || g_velocity_census);', 'facts.frame_direct = copy_alias::g_frame_direct;'),
             ('src/renderer_adapter/binder.h', 'if (facts.frame_direct && facts.fullsize < 3) facts.fullsize = 3;', 'if (facts.frame_direct && facts.fullsize < 3) facts.fullsize = 2;'),
             ('src/renderer_adapter/binder.h', '&& !copy_alias::g_shadow_direct && !copy_alias::g_frame_direct) return;', ') return;'),
             ('src/renderer_adapter/copy_alias.h', 'if (g_fault != Fault::SdNoBlock && facts.shadow_blocked && (facts.shadow_blocked(S) || facts.shadow_blocked(D))) return kWhyExcluded;', ''),
             ('src/renderer_adapter/copy_alias.h', 'if (sd) relax = 2;', ''),
             ('src/renderer_adapter/copy_alias.h', 'const bool sd = facts.shadow_direct && SdPair(facts, copy);', 'const bool sd = facts.shadow_direct;'),
             ('src/renderer_adapter/copy_alias.h', 'inline bool g_frame_direct = false;', 'inline bool g_frame_direct = true;'),
             ('src/fview/session.cpp', 'g_frame_direct) && !rdr_adapter::copy_alias::g_hash_mapped', 'g_hash_mapped) && !rdr_adapter::copy_alias::g_hash_mapped'),
             ('src/fview/main.cpp', '"--frame-direct"', '"--frame-directx"'),
             ('src/renderer/player/rplay.cpp', '"--shadow-direct"', '"--shadow-directx"')]
    for path, old, new in twins:
        if old not in files[path]:
            line(False, 'wiring twin: the text %r is not in %s (the lint is stale: DID NOT APPLY)' % (old[:40], path))
            continue
        twin = dict(files)
        twin[path] = files[path].replace(old, new, 1)
        if twin[path] == files[path]:
            line(False, 'wiring twin %r DID NOT APPLY' % old[:40])
            continue
        line(bool(lint(twin)), 'wiring twin (%s: %r -> %r) is caught' % (os.path.basename(path), old[:36], new[:30]))


# ---- replay arms ------------------------------------------------------------------------------------------------------------------------------------------------------
def core(root, which='bryan'):
    s = setup(root, which)
    if not s:
        return
    exe, cap, game, work = s
    if which == 'bryan':
        A, S = pair(exe, cap, game, work, 'core_sd', BUNDLE, SD, want={'sd': 4.0}, label='bryan shadow direct (the whole bundle)')
        _, F = pair(exe, cap, game, work, 'core_fd', BUNDLE, FD, want={'fd': 4.0}, label='bryan frame direct (the whole bundle)')
        line(F.fd['present'] >= F.n * 0.9, 'frame direct takes the Present\'s copy in %d of %d frames (the frame buffer\'s 1280x720 copy into the picture)' % (F.fd['present'], F.n))
        _, X = pair(exe, cap, game, work, 'core_both', BUNDLE, SD + FD, want={'sd': 4.0, 'fd': 4.0}, label='bryan both on')
    else:
        pair(exe, cap, game, work, which + '_sd', BUNDLE, SD, label='%s shadow direct' % which)
        pair(exe, cap, game, work, which + '_fd', BUNDLE, FD, label='%s frame direct' % which)
        pair(exe, cap, game, work, which + '_both', BUNDLE, SD + FD, label='%s both on' % which)


def default(root):
    s = setup(root, 'bryan')
    if not s:
        return
    exe, cap, game, work = s
    pair(exe, cap, game, work, 'default', DEFAULT3, SD + FD, want={'sd': 4.0, 'fd': 4.0}, label='the DEFAULT bundle (shadows ours + casters + plane + crowd) both on')


def low(root):
    s = setup(root, 'bryan')
    if not s:
        return
    exe, cap, game, work = s
    A, B = pair(exe, cap, game, work, 'low', BUNDLE + LOW, SD + FD, label='default bundle + LOW levers, both on')
    line(B.viol == 0 and B.clean and B.differing(A) == 0, 'LOW (half-rate shadows): shadow direct takes %d and declines %d (counted), stale-read violations %d, pictures equal (%d differ)' % (B.sd['taken'], B.sd['declined'], B.viol, B.differing(A)))


def batch(root):
    s = setup(root, 'bryan')
    if not s:
        return
    exe, cap, game, work = s
    pair(exe, cap, game, work, 'batch', BUNDLE + ['--copy-batch', '1'], SD + FD, want={'sd': 4.0, 'fd': 4.0}, label="G12's copy batch on in both arms")


def g11(root):
    s = setup(root, 'bryan')
    if not s:
        return
    exe, cap, game, work = s
    pair(exe, cap, game, work, 'g11', BUNDLE + ['--frame-selfblit', '--frame-restore'], SD + FD, label="G11's self blit + restore on in both arms")


def plants(root):
    s = setup(root, 'bryan')
    if not s:
        return
    exe, cap, game, work = s
    A = Arm(exe, cap, game, work, 'pl_A', BUNDLE)
    for tag, extra, what, key in (('sd_no_alias', SD + ['--copy-alias-fault', 'sd_no_alias'], 'a shadow reader still bound to the dropped copy\'s destination', 'sd'),
                                  ('fd_no_alias', FD + ['--copy-alias-fault', 'fd_no_alias'], 'the scene copies\' readers still bound to the dropped copies\' destinations', 'fd'),
                                  ('fd_present_stale', FD + ['--copy-alias-fault', 'fd_present_stale'], 'the Present reading a stale image (its copy dropped, the Present not re-pointed)', 'fd')):
        P = Arm(exe, cap, game, work, 'pl_' + tag, BUNDLE + extra, allow_stop=True)
        applied = P.sd['taken'] > 0 if key == 'sd' else P.fd['taken'] > 0
        d = P.differing(A)
        line(applied and (P.stopped or d > 0 or not P.clean), 'twin %s (%s): applied (taken %d), caught: %d of %d frames differ%s, validation %d, exit %d' % (tag, what, P.sd['taken'] if key == 'sd' else P.fd['taken'], d, A.n,
             ', the replay stopped' if P.stopped else '', P.valid, P.rc))
    # the shadow readers re-pointed on a frame their map was not drawn (the half rate): the planted fault lets shadow direct alias the half-rate images
    L0 = Arm(exe, cap, game, work, 'pl_L0', BUNDLE + LOW)
    P = Arm(exe, cap, game, work, 'pl_sd_no_block', BUNDLE + LOW + SD + ['--copy-alias-fault', 'sd_no_block'], allow_stop=True)
    d = P.differing(L0)
    line(P.sd['taken'] > 0 and (P.stopped or d > 0 or not P.clean or P.viol > L0.viol), 'twin sd_no_block (shadow readers re-pointed on frames the half rate did not draw their map): applied (taken %d), caught: %d of %d frames differ%s, stale-read violations %d (the arm with the rule: %d)' %
         (P.sd['taken'], d, L0.n, ', the replay stopped' if P.stopped else '', P.viol, L0.viol))


def half(root):
    """The PLAYERS' default: the whole bundle with the half-rate shadows on (every tier since 3c166f4d), no LOW levers."""
    s = setup(root, 'bryan')
    if not s:
        return
    exe, cap, game, work = s
    A, B = pair(exe, cap, game, work, 'half', BUNDLE + ['--shadow-half'], SD + FD, label="the PLAYERS' default (half-rate shadows on, no LOW levers) both on")
    line(B.viol == 0 and B.sd['taken'] >= 0, 'players\' default: shadow direct takes %.2f a frame (%d declined: %s), frame direct %.2f a frame (%d more by alias_fullsize %d), stale-read violations %d' %
         (B.per(B.sd), B.sd['declined'], B.sd['why'] or '-', B.per(B.fd), B.fd['by_base'], B.fd['level'], B.viol))
    # what the lever takes, by copy (the listing of one frame, `taken 1`)
    for fr in (100, 101):
        L = Arm(exe, cap, game, work, 'half_list%d' % fr, BUNDLE + ['--shadow-half'] + SD + ['--copy-alias-list', str(fr)], frames=fr + 2)
        taken = [m for m in re.findall(r'\[copy_alias\] f%d op (\d+) copy (\d+) -> (\d+) class (\w+) why \'([^\']*)\'.*taken (\d)' % fr, L.text) if m[5] == '1' and m[3] == 'shadow']
        print('      shadow direct under the half rate, frame %d: takes %s' % (fr, ', '.join('op %s %s->%s' % (m[0], m[1], m[2]) for m in taken) or 'nothing'))


def p4(root):
    s = setup(root, 'bryan')
    if not s:
        return
    exe, cap, game, work = s
    pair(exe, cap, game, work, 'p4', BUNDLE + ['--own-shadow-module'], SD, label='P4 --own-shadow-module together with shadow direct')


def readback(root):
    s = setup(root, 'bryan')
    if not s:
        return
    exe, cap, game, work = s
    A = Arm(exe, cap, game, work, 'rb_A', BUNDLE + SD)
    R = Arm(exe, cap, game, work, 'rb_R', BUNDLE + SD + ['--copy-alias-readback', '50:221'])
    line(R.clean and R.differing(A) == 0 and R.sd['hits'] >= 1 and R.per(R.sd) < A.per(A.sd), 'F2 plant: a CPU read-back of texture 221 at frame 50: counted (%d), refused from then on (shadow direct %.2f a frame, was %.2f), pictures equal (%d differ)' %
         (R.sd['hits'], R.per(R.sd), A.per(A.sd), R.differing(A)))
    FA = Arm(exe, cap, game, work, 'rb_FA', BUNDLE + FD)
    FR = Arm(exe, cap, game, work, 'rb_FR', BUNDLE + FD + ['--copy-alias-readback', '50:2,5,1'])
    line(FR.clean and FR.differing(FA) == 0 and FR.fd['hits'] >= 1, 'F2: frame direct counts a read-back of an aliased destination (2 / 5 / 1 at frame 50): %d, pictures equal (%d differ)' % (FR.fd['hits'], FR.differing(FA)))


def partprobe(root):
    s = setup(root, 'bryan')
    if not s:
        return
    exe, cap, game, work = s
    P = Arm(exe, cap, game, work, 'pp_on', BUNDLE + FD + ['--part-probe', '1650'], frames=60)
    line(P.clean and P.probes > 10 and P.probe_bad == 0, 'F3: a Full-mode part of the next frame\'s first 1650 ops after each frame, frame direct on: %d probes, %d last pictures differ' % (P.probes, P.probe_bad))
    Q = Arm(exe, cap, game, work, 'pp_plant', BUNDLE + FD + ['--part-probe', '1650', '--copy-alias-fault', 'fd_no_last_pic_guard'], frames=60)
    line(Q.probes > 10 and Q.probe_bad > 0, 'twin fd_no_last_pic_guard (the part draws into the last picture\'s source with no save): %d of %d last pictures differ' % (Q.probe_bad, Q.probes))


def rplay_val(root):
    """rplay with validation 1 and no per-frame hash: frames really overlap under sync validation."""
    main_co = main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    rplay = os.path.join(root, 'out', 'renderer', 'dev', 'rplay.exe')
    capdir = os.path.join(main_co, BRYAN_COUNTS_DIR)
    p = subprocess.run([rplay, '--capture', capdir, '--frames', '120', '--game', game, '--validation', '1'] + BUNDLE + FD + SD, capture_output=True, text=True, timeout=1700, errors='replace')
    text = p.stdout + p.stderr
    errs = re.findall(r'validation errors? (\d+)', text)
    line(p.returncode == 0 and (not errs or int(errs[-1]) == 0) and FDL.search(text) is not None, 'rplay --validation 1, both on, no per-frame hash (frames overlap): exit %d, validation errors %s, frame direct census read: %s' %
         (p.returncode, errs[-1] if errs else 'none printed', FDL.search(text) is not None))


def unit(root):
    r = subprocess.run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'c3', 'build_c3_unit_test.ps1')], capture_output=True, text=True, timeout=600)
    exe = os.path.join(root, 'out', 'tools', 'c3_unit_test.exe')
    line(r.returncode == 0 and os.path.isfile(exe), 'unit test builds')
    if not os.path.isfile(exe):
        return
    r = subprocess.run([exe], capture_output=True, text=True, timeout=120)
    oks = sum(1 for l in r.stdout.splitlines() if l.startswith('ok'))
    line(r.returncode == 0 and 'all passed' in r.stdout and oks >= 8, 'unit test: F4 (a write after the Present declines), F5 (counted only above the player\'s level), F10 (equal size) pass their %d expectations' % oks)
    for plant in ('fd_no_present_tail', 'sd_size_blind'):
        r = subprocess.run([exe, '--plant', plant], capture_output=True, text=True, timeout=120)
        first = next((l for l in r.stdout.splitlines() if l.startswith('FAIL')), '')
        line(r.returncode == 1, 'unit plant %s is caught: %s' % (plant, first[:100] if first else 'NOT CAUGHT'))


# ---- counts ---------------------------------------------------------------------------------------------------------------------------------------------------------
def rplay_counts(rplay, capdir, game, work, name, extra, frames=100):
    out = os.path.join(work, name + '.json')
    p = subprocess.run([rplay, '--capture', capdir, '--frames', str(frames), '--game', game, '--counts', out] + BUNDLE + extra, capture_output=True, text=True, timeout=1700, errors='replace')
    if p.returncode != 0:
        raise RuntimeError('rplay %s exit %d: %s' % (name, p.returncode, (p.stdout + p.stderr)[-800:]))
    acc = dict(copies=0, barriers=0, passes=0, binds=0, draws=0)
    n = 0
    with open(out, encoding='utf-8') as f:
        for ln in f:
            ln = ln.strip().rstrip(',')
            if ln.startswith('{"frame"'):
                d = json.loads(ln)
                if d['frame'] >= 10:   # (the first frames carry the start-up uploads)
                    acc['copies'] += d['total']['copies']
                    acc['barriers'] += d['total']['barriers']
                    acc['binds'] += d['total']['pipeline_binds']
                    acc['draws'] += d['total']['draws'] + d['total']['indexed_draws']
                    acc['passes'] += d['passes']
                    n += 1
    return {k: v / max(1, n) for k, v in acc.items()}, p.stdout + p.stderr


def counts(root):
    main_co = main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    rplay = os.path.join(root, 'out', 'renderer', 'dev', 'rplay.exe')
    capdir = os.path.join(main_co, BRYAN_COUNTS_DIR)
    work = os.path.join(root, 'out', 'c3_check')
    os.makedirs(work, exist_ok=True)
    if not (os.path.isfile(rplay) and os.path.isdir(capdir)):
        line(False, 'rplay or its capture is missing: %s %s' % (rplay, capdir))
        return
    K = ['--copy-batch', '1']   # G12's copy batch ON in every arm (the merged lever that touches the same work)
    base, _ = rplay_counts(rplay, capdir, game, work, 'cnt_A', K)
    rows = [('base (bundle + copy batch)', base)]
    for tag, extra, key in (('shadow direct', SD, 'sd'), ('frame direct', FD, 'fd'), ('both', SD + FD, 'both')):
        c, text = rplay_counts(rplay, capdir, game, work, 'cnt_' + key, K + extra)
        rows.append((tag, c))
        msd = SDL.search(text)
        mfd = FDL.search(text)
        taken = (int(msd.group(2)) if msd and key in ('sd', 'both') else 0) + (int(mfd.group(3)) if mfd and key in ('fd', 'both') else 0)
        per = taken / 100.0
        fell = base['copies'] - c['copies']
        line(taken >= 3 * 100 and abs(fell - per) < 0.6 and c['passes'] <= base['passes'] + 0.01 and c['barriers'] <= base['barriers'] + 0.01 and c['binds'] <= base['binds'] + 0.01,
             '%s (rplay, 100 frames, bundle + copy batch in both arms): taken %.2f a frame, copies %.2f -> %.2f (-%.2f), passes %.2f -> %.2f, barriers %.2f -> %.2f (%+.2f), pipeline binds %.2f -> %.2f' %
             (tag, per, base['copies'], c['copies'], fell, base['passes'], c['passes'], base['barriers'], c['barriers'], c['barriers'] - base['barriers'], base['binds'], c['binds']))
    # F5: under the LOW levers (alias_fullsize 1) frame direct is only what level 3 adds: the Present's copy, delta 1
    lb, _ = rplay_counts(rplay, capdir, game, work, 'cnt_LA', K + LOW)
    lf, lt = rplay_counts(rplay, capdir, game, work, 'cnt_LF', K + LOW + FD)
    mf = FDL.search(lt)
    per = int(mf.group(3)) / 100.0 if mf else -1
    line(mf is not None and abs((lb['copies'] - lf['copies']) - 1.0) < 0.35 and abs(per - 1.0) < 0.05 and lf['passes'] <= lb['passes'] + 0.01 and lf['barriers'] <= lb['barriers'] + 0.01,
         'F5 (LOW levers, alias_fullsize 1, copy batch in both arms): frame direct copies %.2f -> %.2f (delta %.2f, wanted 1), census counts %.2f a frame as frame_direct\'s (%d more already by alias_fullsize), passes %.2f -> %.2f, barriers %.2f -> %.2f' %
         (lb['copies'], lf['copies'], lb['copies'] - lf['copies'], per, int(mf.group(7)) if mf else -1, lb['passes'], lf['passes'], lb['barriers'], lf['barriers']))
    # the PLAYERS' default: half-rate shadows on (every tier), no LOW levers: what each switch takes there, by counts
    H = ['--shadow-half']
    hb, _ = rplay_counts(rplay, capdir, game, work, 'cnt_HA', K + H)
    for tag, extra, key in (('shadow direct', SD, 'sd'), ('frame direct', FD, 'fd')):
        hc, ht = rplay_counts(rplay, capdir, game, work, 'cnt_H' + key, K + H + extra)
        mm = (SDL if key == 'sd' else FDL).search(ht)
        per = (int(mm.group(2)) if key == 'sd' else int(mm.group(3))) / 100.0 if mm else -1
        fell = hb['copies'] - hc['copies']
        line(mm is not None and per >= 2.5 and abs(fell - per) < 0.6 and hc['passes'] <= hb['passes'] + 0.01 and hc['barriers'] <= hb['barriers'] + 0.01 and hc['binds'] <= hb['binds'] + 0.01,
             "PLAYERS' default (half-rate shadows on, copy batch in both arms) %s: taken %.2f a frame, copies %.2f -> %.2f (-%.2f), passes %.2f -> %.2f, barriers %.2f -> %.2f, pipeline binds %.2f -> %.2f" %
             (tag, per, hb['copies'], hc['copies'], fell, hb['passes'], hc['passes'], hb['barriers'], hc['barriers'], hb['binds'], hc['binds']))
    with open(os.path.join(work, 'counts_table.txt'), 'w') as f:
        for tag, c in rows:
            f.write('%-30s copies %.2f passes %.2f barriers %.2f binds %.2f draws %.2f\n' % (tag, c['copies'], c['passes'], c['barriers'], c['binds'], c['draws']))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--only', default='')
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    o = args.only
    if o in ('', 'wiring'):
        wiring(root)
    if o in ('', 'core'):
        core(root, 'bryan')
    if o in ('', 'default'):
        default(root)
    if o in ('', 'low'):
        low(root)
    if o in ('', 'batch'):
        batch(root)
    if o in ('', 'g11'):
        g11(root)
    if o in ('', 'plants'):
        plants(root)
    if o in ('', 'unit'):
        unit(root)
    if o in ('', 'half'):
        half(root)
    if o in ('', 'p4'):
        p4(root)
    if o in ('', 'readback'):
        readback(root)
    if o in ('', 'partprobe'):
        partprobe(root)
    if o in ('', 'rplayval'):
        rplay_val(root)
    if o in ('', 'match'):
        core(root, 'match')
    if o in ('', 'brock'):
        core(root, 'brock')
    if o in ('', 'counts'):
        counts(root)
    print('C3: %s' % ('all passed' if not fails else '%d FAILED' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
