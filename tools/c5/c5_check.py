#!/usr/bin/env python3
"""S102 lane C5: copy_direct (docs/renderer/COPIES_LEFT.md section 6). Offline, 0 game runs, 0 Surface runs.

  py -I tools\\c5\\c5_check.py [--root DIR] [--only wiring|bryan|match|counts]

The switch wwe2k14_rdr_copy_direct (PerFrame, default off; fview / rplay --copy-direct 1) REMOVES the crowd card bake's 20 copies and the small sprite / post stand-ins' copies by drawing the pass
straight into the copy's destination (copy_alias.h Plan.direct, binder.h DirectCheck / IssueTargets / IssueCopy). Arms (S102 common.md "Speed of the lane itself": the player config on / off, the planted
faults, ONE other capture):
  wiring  every link of the switch in the source (table PerFrame default 0, cvar default false, live.cpp per-frame read, fview / rplay flags, binder drop + retarget); each removed in a text copy must fail the lint;
          each text plant checks it applied (a DID NOT APPLY failure otherwise).
  bryan   fview headless, every frame hashed, validation errors AND exit code read: the player config (the whole bundle) off vs on = byte-equal on all frames, the census line says the lever took copies
          (taken > 0 a frame); the LOW combination (copy alias 2, classes 11) on vs off; the planted twins must differ: no_retarget (the destination is never written: a reader still bound to a removed copy's
          destination, also what a CPU read of it sees), wrong_dest (a bake written to the wrong atlas cell), and a copy removed while its destination is read back by the CPU (the planted read-back
          refuses it: the census declines it and pictures stay equal).
  match   the same pair on the standard match capture (the one other capture).
  counts  rplay --counts: copies / barriers a frame off vs on: the copies fall by what the census says, barriers fall with them, draws, clears and pipeline binds do not rise."""
import argparse
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'g12'))
import g12_check as g  # noqa: E402  (Arm, BUNDLE, LOW, CAPTURES, main_checkout, rplay_counts: the same harness)

fails = 0
DIRECT = re.compile(r'\[rdr\]\[copy_direct\] frames (\d+), taken (\d+) \(([\d.]+) MPix; bake (\d+), other (\d+)\), declined (\d+), stale-read violations (\d+)')


def line(ok, text):
    global fails
    print(('PASS  ' if ok else 'FAIL  ') + text, flush=True)
    fails += 0 if ok else 1


def census(arm):
    m = DIRECT.search(arm.text)
    if not m:
        return dict(frames=0, taken=0, bake=0, other=0, declined=0, per=0.0, viol=-1)
    d = dict(frames=int(m.group(1)), taken=int(m.group(2)), bake=int(m.group(4)), other=int(m.group(5)), declined=int(m.group(6)), viol=int(m.group(7)))
    d['per'] = d['taken'] / max(1, d['frames'])
    return d


# ---- unit -----------------------------------------------------------------------------------------------------------------------------------------------------------
def unit(root):
    r = subprocess.run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'c5', 'build_copy_direct_test.ps1')], capture_output=True, text=True, timeout=600)
    exe = os.path.join(root, 'out', 'tools', 'copy_direct_test.exe')
    line(r.returncode == 0 and os.path.isfile(exe), 'unit test builds')
    if not os.path.isfile(exe):
        return
    r = subprocess.run([exe], capture_output=True, text=True, timeout=120)
    oks = sum(1 for l in r.stdout.splitlines() if l.startswith('ok'))
    line(r.returncode == 0 and 'all passed' in r.stdout and oks >= 17, 'unit test: the shipped rule passes its %d expectations (a pass with no clear, a copy into the source, a Present of it, a banned source, a sub-rect copy ... each declined)' % oks)
    for plant in ('no_clear', 'no_copy_into', 'no_present', 'no_whole', 'no_ledger', 'no_shared', 'no_scale'):
        r = subprocess.run([exe, '--plant', plant], capture_output=True, text=True, timeout=120)
        first = next((l for l in r.stdout.splitlines() if l.startswith('FAIL')), '')
        line(r.returncode == 1, 'unit plant %s is caught: %s' % (plant, first[:90] if first else 'NOT CAUGHT'))
    # the fault table order (review F3): the header compiles; the same header with two names swapped does not
    clang = r'C:\Program Files\LLVM\bin\clang++.exe'
    work = os.path.join(root, 'out', 'c5_check', 'faults')
    os.makedirs(work, exist_ok=True)
    src = open(os.path.join(root, 'src', 'renderer_adapter', 'copy_alias.h'), encoding='utf-8').read()
    a, b = '{Fault::FsNoReader, "fs_no_reader"}', '{Fault::SdNoBlock, "sd_no_block"}'
    swapped = src.replace(a, '@@A@@').replace(b, a).replace('@@A@@', b)
    if swapped == src or a not in src or b not in src:
        line(False, 'fault-table plant DID NOT APPLY')
        return
    flags = [clang, '-std=c++20', '-fsyntax-only', '-Wno-deprecated-declarations', '-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-I', os.path.join(root, 'src', 'renderer'), '-I', os.path.join(root, 'src', 'renderer_adapter'), '-I', os.path.join(root, 'src')]
    for tag, text in (('good', src), ('swapped', swapped)):
        d = os.path.join(work, tag)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, 'copy_alias.h'), 'w', encoding='utf-8', newline='') as f:
            f.write(text)
        with open(os.path.join(d, 'stub.cpp'), 'w') as f:
            f.write('#include "copy_alias.h"\nint main() { return 0; }\n')
    ok = subprocess.run(flags + [os.path.join(work, 'good', 'stub.cpp')], capture_output=True, text=True, timeout=300)
    bad = subprocess.run(flags + [os.path.join(work, 'swapped', 'stub.cpp')], capture_output=True, text=True, timeout=300)
    line(ok.returncode == 0, 'the fault table compiles as shipped')
    line(bad.returncode != 0 and 'enum order' in (bad.stderr + bad.stdout), 'plant: two fault names swapped (fs_no_reader / sd_no_block) does not compile ("each paired with its own enumerator")')


# ---- wiring ---------------------------------------------------------------------------------------------------------------------------------------------------------
FILES = ('src/renderer_adapter/rdr_switches.h', 'src/renderer_adapter/reader.h', 'src/renderer_adapter/live.cpp', 'src/renderer_adapter/binder.h', 'src/renderer_adapter/copy_alias.h',
         'src/fview/main.cpp', 'src/renderer/player/rplay.cpp')


def lint(fs):
    bad = []
    t = fs['src/renderer_adapter/rdr_switches.h']
    m = re.search(r'X\(CopyDirect, "wwe2k14_rdr_copy_direct", (\w+), (\d+)\)', t)
    if not m or m.group(1) != 'PerFrame' or m.group(2) != '0':
        bad.append('table line CopyDirect (PerFrame, default 0)')
    if 'REXCVAR_DEFINE_BOOL(wwe2k14_rdr_copy_direct, false,' not in fs['src/renderer_adapter/reader.h']:
        bad.append('cvar copy_direct default false')
    if 'copy_alias::g_direct = copy_alias::ClampDirect(rdr_sw::BoolSeen(rdr_sw::kCopyDirect' not in fs['src/renderer_adapter/live.cpp']:
        bad.append('live.cpp reads copy_direct every frame')
    if '"--copy-direct"' not in fs['src/fview/main.cpp'] or '"--copy-direct"' not in fs['src/renderer/player/rplay.cpp']:
        bad.append('fview / rplay flag --copy-direct')
    b = fs['src/renderer_adapter/binder.h']
    if 'dr_on_ && !synthetic && op_index < dr_copy_.size() && dr_copy_[op_index]' not in b:
        bad.append('IssueCopy drops a direct copy (and never a synthetic restore copy)')
    if 'if (dr_on_ && !retarget && targets.color_count && op_index < dr_set_dest_.size() && dr_set_dest_[op_index]) targets.color[0].image = Map(dr_set_dest_[op_index]);' not in b:
        bad.append('IssueTargets retargets the pass to the destination')
    if 'facts.direct_level = (g_dlss || g_velocity_census || velocity_mode_) ? 0 :' not in b:
        bad.append('no direct pass under DLSS / velocity')
    if '&& copy_alias::g_direct <= 0) return;' not in b:
        bad.append('AliasBegin runs the planner for the direct switch alone')
    if 'f.dest_read_back = copy_alias::WasReadBack(D);' not in b or 'f.source_sampled = (src.usage & rdr::kUsageSampled) != 0;' not in b:
        bad.append('DirectCheck: source never sampled, destination never read back')
    if 'f.dest_target_capable = dr_target_ids_.count(D) != 0;' not in b:
        bad.append('DirectCheck: destination made with target usage')
    if 'if (frame.ops[i].kind != OpKind::Clear && frame.ops[i].kind != OpKind::Draw) f.only_clears_draws = false;' not in b:
        bad.append('DirectCheck: nothing between the pass and its copy but clears and draws')
    if 'f.first_op_full_clear = c.full && (c.flags & rdr::kClearColor) && (c.color_mask & 1u);' not in b:
        bad.append('DirectCheck: the pass starts with a full colour clear (review F1)')
    if 'const int why = copy_direct::Decide(f);' not in b or 'if (S < dr_stale_d_.size()) dr_stale_d_[S] = D;' not in b:
        bad.append('DirectCheck: the pure rule decides and a taken source enters the stale-read ledger')
    if 'read_before_draw(cp.source.image, *e);' not in b or 'read_before_draw(id, *e);' not in b:
        bad.append('the ledger sees copy and Present reads of a taken source before its pass (review F1)')
    if 'if (facts.direct_level > 0) DirectScan(frame);' not in b:
        bad.append('the scan runs on planned frames')
    if 'if (e->sets < 255) ++e->sets;' not in b or 'e->copy_dests' not in b:
        bad.append('the scan counts passes, copy sources and copy destinations of every image')
    if 'std::vector<DrScan> dr_scan_;' not in b:
        bad.append('F6: a flat scan table (no unordered_map)')
    c = fs['src/renderer_adapter/copy_alias.h']
    if 'inline int g_direct = 0;' not in c:
        bad.append('copy_alias.h direct default 0')
    if 'plan.direct[p] = 1, ++plan.direct_count;' not in c:
        bad.append('planner marks direct copies')
    if 'DirectFaultOk(g_fault) ? facts.direct_check(p, copy) : int(copy_direct::kDirectFault)' not in c:
        bad.append('the planner declines under a planted alias fault (review F2)')
    if 'static_assert(FaultNamesInOrder()' not in c:
        bad.append('the fault table is checked in enum order at compile time (review F3)')
    return bad


def wiring(root):
    fs = {}
    for f in FILES:
        with open(os.path.join(root, f), encoding='utf-8', errors='replace') as h:
            fs[f] = h.read()
    bad = lint(fs)
    line(not bad, 'wiring: every link of copy_direct is in the source%s' % ((' (missing: ' + '; '.join(bad) + ')') if bad else ''))
    plants = [
        ('src/renderer_adapter/rdr_switches.h', 'X(CopyDirect, "wwe2k14_rdr_copy_direct", PerFrame, 0)', 'X(CopyDirect, "wwe2k14_rdr_copy_direct", Startup, 1)'),
        ('src/renderer_adapter/reader.h', 'REXCVAR_DEFINE_BOOL(wwe2k14_rdr_copy_direct, false,', 'REXCVAR_DEFINE_BOOL(wwe2k14_rdr_copy_direct, true,'),
        ('src/renderer_adapter/live.cpp', 'rdr_sw::BoolSeen(rdr_sw::kCopyDirect', 'rdr_sw::BoolOr(rdr_sw::kCopyDirect'),
        ('src/renderer_adapter/binder.h', '&& !synthetic && op_index < dr_copy_.size()', '&& op_index < dr_copy_.size()'),
        ('src/renderer_adapter/binder.h', 'f.dest_target_capable = dr_target_ids_.count(D) != 0;', 'f.dest_target_capable = true;'),
        ('src/renderer_adapter/binder.h', 'f.dest_read_back = copy_alias::WasReadBack(D);', 'f.dest_read_back = false;'),
        ('src/renderer_adapter/binder.h', 'f.first_op_full_clear = c.full && (c.flags & rdr::kClearColor) && (c.color_mask & 1u);', 'f.first_op_full_clear = true;'),
        ('src/renderer_adapter/binder.h', 'read_before_draw(cp.source.image, *e);', ''),
        ('src/renderer_adapter/binder.h', 'std::vector<DrScan> dr_scan_;', 'std::unordered_map<uint32_t, DrScan> dr_scan_;'),
        ('src/renderer_adapter/copy_alias.h', 'DirectFaultOk(g_fault) ? facts.direct_check(p, copy) : int(copy_direct::kDirectFault)', 'facts.direct_check(p, copy)'),
    ]
    for f, old, new in plants:
        text = fs[f].replace(old, new, 1)
        if text == fs[f]:
            line(False, 'wiring plant DID NOT APPLY (%s: %s)' % (f, old[:50]))
            continue
        twin = dict(fs)
        twin[f] = text
        line(bool(lint(twin)), 'wiring plant caught (%s: %s)' % (os.path.basename(f), old[:56]))


# ---- replay ---------------------------------------------------------------------------------------------------------------------------------------------------------
def replay(root, which):
    main_co = g.main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    sys.path.insert(0, os.path.join(root, 'tools', 'fview'))
    import fview_exe as fe
    exe = fe.find_or_exit(root=root, kind='dev')
    work = os.path.join(root, 'out', 'c5_check')
    os.makedirs(work, exist_ok=True)
    z = os.path.join(main_co, g.CAPTURES[which])
    if not os.path.exists(z):
        line(False, 'input missing: ' + z)
        return
    n = which
    B = g.BUNDLE
    A = g.Arm(exe, z, game, work, n + '_A', B)
    line(A.clean and len(A.hashes) > 1, 'baseline %s (the player config: the whole bundle, copy_direct off): %d frames hashed, validation %d, exit %d' % (n, A.frames_hashed, A.valid, A.rc))
    D = g.Arm(exe, z, game, work, n + '_D', B + ['--copy-direct', '1'])
    c = census(D)
    want = 15.0 if n == 'bryan' else 0.9   # (the match's 15 bake copies have no reader: the strict rule leaves them to G4; the one copy a frame taken is the sprite stand-in that clears fully)
    line(D.differing(A) == 0 and len(D.hashes) == len(A.hashes) and D.clean and c['per'] >= want and c['viol'] == 0,
         'copy_direct %s: %d frames, %d differ, validation %d, exit %d; the lever took %d copies in %d frames (%.2f a frame; bake %d, other %d), declined %d, stale-read violations %d (must be 0)' %
         (n, D.frames_hashed, D.differing(A), D.valid, D.rc, c['taken'], c['frames'], c['per'], c['bake'], c['other'], c['declined'], c['viol']))
    if n != 'bryan':
        return
    # F4: a render scale other than 1 (the copies of a picture that follows the scale are judged by the same rule)
    S0 = g.Arm(exe, z, game, work, n + '_S0', B + ['--scale', '0.75'])
    S1 = g.Arm(exe, z, game, work, n + '_S1', B + ['--scale', '0.75', '--copy-direct', '1'])
    cs = census(S1)
    line(S1.differing(S0) == 0 and S0.clean and S1.clean and cs['per'] >= want and cs['viol'] == 0, 'render scale 0.75 %s: %d differ, validation %d/%d, exit %d/%d, took %.2f copies a frame' % (n, S1.differing(S0), S0.valid, S1.valid, S0.rc, S1.rc, cs['per']))
    # F2: a planted alias fault admits copies the strict rule would refuse: the direct switch declines them (nothing taken), pictures equal, validation 0
    for fault in ('sub_rect', 'read_first'):
        F = g.Arm(exe, z, game, work, n + '_F_' + fault, B + ['--copy-direct', '1', '--copy-alias-fault', fault])
        cf = census(F)
        line(F.differing(A) == 0 and F.clean and cf['taken'] == 0 and cf['declined'] > 0, 'alias fault %s with copy_direct on: declined (taken %d, declined %d), %d frames differ, validation %d, exit %d' % (fault, cf['taken'], cf['declined'], F.differing(A), F.valid, F.rc))
    L0 = g.Arm(exe, z, game, work, n + '_L0', B + g.LOW)
    L1 = g.Arm(exe, z, game, work, n + '_L1', B + g.LOW + ['--copy-direct', '1'])
    cl = census(L1)
    line(L1.differing(L0) == 0 and L0.clean and L1.clean and cl['per'] >= want, 'the LOW combination (copy alias 2, classes 11, shadow 256 half) %s: %d differ, validation %d/%d, exit %d/%d, took %.2f copies a frame' %
         (n, L1.differing(L0), L0.valid, L1.valid, L0.rc, L1.rc, cl['per']))
    for fault, what in (('direct_no_retarget', 'a reader still bound to a removed copy\'s destination (the pass keeps drawing into the source: the destination is never written)'),
                        ('direct_wrong_dest', 'a bake written to the wrong atlas cell (each pass renders into the next pass\'s destination)'),
                        ('direct_stale_src', 'frame N+1 keeps the copy but the pass already draws into the destination: the copy reads a source nobody writes')):
        P = g.Arm(exe, z, game, work, n + '_P_' + fault, B + ['--copy-direct', '1', '--copy-alias-fault', fault], allow_stop=True)
        dd = P.differing(A)
        line((P.stopped and '[rdr][PROBLEM]' in P.text) or dd > 0, 'twin %s is caught: %s: %d frames differ%s' % (fault, what, dd, ', the replay stopped on [PROBLEM]' if P.stopped else ''))
    # the stale-read ledger: shown a read of every taken source before its pass, it bans them and the lever stops taking (violations > 0, taken falls, pictures equal)
    Q = g.Arm(exe, z, game, work, n + '_Q', B + ['--copy-direct', '1', '--copy-alias-fault', 'direct_read_src'])
    cq = census(Q)
    line(Q.differing(A) == 0 and Q.clean and cq['viol'] >= 1 and cq['per'] < c['per'] - 1.0, 'ledger twin direct_read_src: %d sources banned, the lever takes %.2f copies a frame (was %.2f), pictures equal, validation %d' % (cq['viol'], cq['per'], c['per'], Q.valid))
    # a CPU read-back of destinations the switch takes (the bake pictures): the switch refuses them from then on and the pictures stay equal
    R = g.Arm(exe, z, game, work, n + '_R', B + ['--copy-direct', '1', '--copy-alias-readback', '30:414,415,416,417,418,419,420,421,422,423'])
    cr = census(R)
    line(R.differing(A) == 0 and R.clean and cr['per'] < c['per'] - 1.0, 'a planted CPU read-back of ten destinations at frame 30: refused from then on (%.2f copies a frame, was %.2f), pictures equal (%d differ), validation %d' % (cr['per'], c['per'], R.differing(A), R.valid))


# ---- counts ---------------------------------------------------------------------------------------------------------------------------------------------------------
def totals(path):
    rows = []
    with open(path, encoding='utf-8') as f:
        for ln in f:
            ln = ln.strip().rstrip(',')
            if ln.startswith('{"frame"'):
                d = json.loads(ln)
                if d['frame'] >= 10:
                    rows.append(d['total'])
    return {k: sum(r[k] for r in rows) / len(rows) for k in rows[0]}


def counts(root):
    main_co = g.main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    rplay = os.path.join(root, 'out', 'renderer', 'dev', 'rplay.exe')
    capdir = os.path.join(main_co, g.BRYAN_COUNTS_DIR)
    work = os.path.join(root, 'out', 'c5_check')
    os.makedirs(work, exist_ok=True)
    if not (os.path.isfile(rplay) and os.path.isdir(capdir)):
        line(False, 'rplay or its capture is missing: %s %s' % (rplay, capdir))
        return
    _, _, _ = g.rplay_counts(rplay, capdir, game, work, 'cnt_A', [])
    _, _, tb = g.rplay_counts(rplay, capdir, game, work, 'cnt_D', ['--copy-direct', '1'])
    a = totals(os.path.join(work, 'cnt_A.json'))
    b = totals(os.path.join(work, 'cnt_D.json'))
    m = DIRECT.search(tb)
    taken = int(m.group(2)) / 100.0 if m else -1
    keys = ('copies', 'barriers', 'draws', 'clears', 'pipeline_binds')
    d = {k: b[k] - a[k] for k in keys if k in a}
    line(m is not None and a['copies'] - b['copies'] >= 20 and abs((a['copies'] - b['copies']) - taken) < 0.5 and a['barriers'] - b['barriers'] >= 20 and b['draws'] <= a['draws'] + 0.01 and b['clears'] <= a['clears'] + 0.01 and b['pipeline_binds'] <= a['pipeline_binds'] + 0.01,
         'copy_direct counts (rplay, 100 frames, the bundle): copies %.2f -> %.2f (%+.2f), barriers %.2f -> %.2f (%+.2f), draws %+.2f, clears %+.2f (a pass is a SetTargets + its draws: none added), pipeline binds %+.2f; the census took %.2f a frame' %
         (a['copies'], b['copies'], d['copies'], a['barriers'], b['barriers'], d['barriers'], d['draws'], d['clears'], d['pipeline_binds'], taken))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--only', default='')
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    if args.only in ('', 'unit'):
        unit(root)
    if args.only in ('', 'wiring'):
        wiring(root)
    if args.only in ('', 'bryan'):
        replay(root, 'bryan')
    if args.only in ('', 'match'):
        replay(root, 'match')
    if args.only in ('', 'counts'):
        counts(root)
    print('C5: %s' % ('all passed' if not fails else '%d FAILED' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
