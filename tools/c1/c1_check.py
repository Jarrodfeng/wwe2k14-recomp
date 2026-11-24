r"""S102 lane C1 offline proof, 0 game runs, 0 Surface runs: the pre-light strips' six copies GONE, one multiview pass into the cube's six layers (src\renderer_adapter\prelight_layered.h,
switch wwe2k14_rdr_prelight_layered; vertex program yPreLight_base_layered; renderer: src\renderer\layered_targets.h).
  py -I tools\c1\c1_check.py [--root DIR] [--only census,wiring,replay,match,counts,plants] [--captures bryan,brock]

fview sweeps (--sweep: the picture and the hash of EVERY layer of every cube image bound at fetch slot 13 -- the cubes the characters sample -- on every frame; the frame the renderer is given: ops / targets /
draws / copies counts) on the Bryan capture and on Check B (brock, 341 frames), with G12's copy batch ON in both arms. The faces are drawn ours-vs-ours (the same owned pre-light mesh in both arms),
so the bar is BYTE-EQUAL pictures and cubes. Arms (switch off = A, on = B), every one with `--small-passes-ours --copy-batch on --validation 1`:
  main      A vs B: pictures and cube hashes equal on every frame; validation read from the error count AND the exit code; every group taken, none declined, none fell back; passes (targets) a frame
            UNCHANGED; copies a frame fall by exactly 6 x taken groups; layered passes = taken; strip passes not made = taken.
  alias2    --copy-alias 2 on both: B equals A and equals the main B (the alias end rules; the copies are an op another hook drops).
  bundle    the shipped default bundle (--shadows-ours --shadow-casters-ours --plane-shadow-ours): B equals A, and the lever TOOK frames (taken > 0 groups, frames taken > 0).
  hook_off  --prelight-layered without --small-passes-ours: every group declined (small_passes_ours is off), copies unchanged, equal.
  nomv      --no-multiview (the device has none): every group declined for it (counted), the stream keeps its strip and copies, equal.
  fallback  --replace-fault small.cube_last_vertex (the owned hook refuses every pre-light draw): every group falls back after the fact (counter), copies unchanged, pictures equal.
  dead      --dead-work 2 --dead-work-fault cut_live (G4's pre-light cut made real): the cut slots are declined as dead, the rest taken, no op both cut and taken.
Planted twins (each must be CAUGHT by the check named): swap_faces (cube hashes differ), missing_layer (they differ), keep_copies (copies do not fall), fallback_no_strip with --no-multiview (the fallback
taken but the strip not made: the faces differ), phantom_reader + no_strip_check (taken with a reader > 0), ignore_overlap with the dead cut (taken overlapping > 0).
match: the match capture (the case zip) replayed under the whole bundle with the switch off / on: pictures equal on every frame, validation clean.
counts: rplay renderer counts on Bryan, copy batch ON in both arms: copies, passes, barriers, pipeline binds a frame (passes and barriers must not rise; copies fall by 5 against the batch).
Also: the face map from the captures alone (c1_census.py: tile j -> layer j, no flip), the wiring (switch table, cvar, per-frame read with the guest frame, helper calls, census line, target usage) with each link
removed in a text copy. Exit 1 on any FAIL; a missing capture or exe FAILS."""
import argparse
import csv
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
FAILS = 0
MATCH_CASE = 'out/s101/runs/20261112_131008_s100_owner_match/case_20261112_131027_265_0_pc_run_exit.zip'
BRYAN_COUNTS_DIR = 'out/s101/lane92/case_20261112_122719_969_0_pc_run_exit/capture'
BUNDLE = ['--crowd-ours', '--crowd-cards', '--arena-ours', '--wrestlers-ours', '--caw-ours', '--ring-ours', '--ui-ours', '--ui-quads', '--ui-text-ours', '--flash-ours', '--props-ours', '--post-ours',
          '--movie-ours', '--screens-ours', '--shadow-casters-ours', '--rim-ours', '--circles-ours', '--plane-shadow-ours', '--small-passes-ours', '--rim-chain-ours', '--particles-ours', '--shadows-ours']


def line(ok, text):
    global FAILS
    print(('PASS  ' if ok else 'FAIL  ') + text, flush=True)
    FAILS += 0 if ok else 1


def main_checkout(root):
    env = os.environ.get('RDR_MAIN_CHECKOUT')
    if env:
        return env
    try:
        common = subprocess.run(['git', '-C', root, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return root


class Env:
    def __init__(self, root):
        self.root = root
        self.main = main_checkout(root)
        self.fview = os.path.join(root, 'out', 'renderer', 'dev', 'fview.exe')
        if not os.path.exists(self.fview):
            self.fview = os.path.join(self.main, 'out', 'renderer', 'dev', 'fview.exe')
        self.rplay = os.path.join(root, 'out', 'renderer', 'dev', 'rplay.exe')
        if not os.path.exists(self.rplay):
            self.rplay = os.path.join(self.main, 'out', 'renderer', 'dev', 'rplay.exe')
        self.fview_rel = os.path.join(root, 'out', 'renderer', 'release', 'fview.exe')   # the player build (RDR_DEV off): a Problem is logged once and the run goes on; the dev exe aborts on every Problem
        self.work = os.path.join(root, 'out', 'c1')
        os.makedirs(self.work, exist_ok=True)
        self.caps = {'bryan': os.path.join(self.main, 'out', 'lane86', 'bryan', 'capture'), 'brock': os.path.join(self.main, 'out', 'lane86', 'brock', 'capture')}
        self.game = os.path.join(self.main, 'game_data', 'extracted')


CENSUS = re.compile(r'\[rdr\]\[prelight_layered\] frames (\d+) \(with strips (\d+), taken (\d+)\), groups (\d+), taken (\d+), declined (\d+)(?: \((.*?)\))? \| layered passes (\d+), strip passes not made (\d+), '
                    r'copies dropped (\d+), fell back after the fact \(draw not ours\) (\d+) \| audit: taken with a reader (\d+), taken overlapping (\d+)')
KEYS = ('frames', 'with_strips', 'frames_taken', 'groups', 'taken', 'declined', 'why', 'layered', 'unmade', 'dropped', 'fallback', 'with_reader', 'overlapping')


def parse_census(text):
    m = CENSUS.search(text)
    return dict(zip(KEYS, [x if i == 6 else int(x or 0) for i, x in enumerate(m.groups())])) if m else dict.fromkeys(KEYS, 0), bool(m)


class Arm:
    def __init__(self, env, cap, name, extra, base=('--small-passes-ours', '--copy-batch', 'on'), exe=None):
        out = os.path.join(env.work, '%s_%s.csv' % (cap, name))
        if os.path.exists(out):
            os.remove(out)
        e = dict(os.environ, FVIEW_SWEEP_COUNTS='1', FVIEW_SWEEP_LAYERS='1')
        cmd = [exe or env.fview, '--capture', env.caps[cap], '--validation', '1', '--sweep', out, '--sweep-slot', '13'] + list(base) + list(extra)
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=1800, env=e, errors='replace')
        self.text = (p.stdout or '') + (p.stderr or '')
        with open(os.path.join(env.work, '%s_%s.log' % (cap, name)), 'w', encoding='utf-8') as f:
            f.write(self.text)
        self.rc = p.returncode
        v = re.findall(r'validation errors (\d+)', self.text)
        self.errors = int(v[-1]) if v else -1
        self.rows = list(csv.DictReader(open(out))) if os.path.exists(out) else []
        n = max(1, len(self.rows))
        self.copies = sum(int(r['gpu_copies']) for r in self.rows) / n
        self.targets = sum(int(r['gpu_targets']) for r in self.rows) / n
        self.draws = sum(int(r['gpu_draws']) for r in self.rows) / n
        self.c, self.census_line = parse_census(self.text)

    def valid(self):
        return self.rc == 0 and self.errors == 0 and bool(self.rows)

    def diff(self, other):
        """-> (frames whose picture differs, frames whose cube hash differs, frame count mismatch)"""
        n = min(len(self.rows), len(other.rows))
        pic = sum(1 for i in range(n) if self.rows[i]['picture'] != other.rows[i]['picture'])
        cube = sum(1 for i in range(n) if self.rows[i]['slot13_hash'] != other.rows[i]['slot13_hash'])
        return pic, cube, len(self.rows) != len(other.rows)


def clean(a, text):
    line(a.valid(), '%s: validation clean (error count %d, exit code %d, %d frames)' % (text, a.errors, a.rc, len(a.rows)))


def equal(a, b, text):
    pic, cube, mism = a.diff(b)
    line(pic == 0 and cube == 0 and not mism, '%s: pictures %d and cube layers %d frames differ of %d' % (text, pic, cube, len(a.rows)))


def census_check():
    r = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'c1_census.py')] + [os.path.join(main_checkout(ROOT), 'out', 'lane86', c, 'capture') for c in ('bryan', 'brock')], capture_output=True, text=True, timeout=1800)
    print(r.stdout.strip())
    line(r.returncode == 0 and 'MAP identity' in r.stdout, 'face map from the captures: tile j -> layer j, no flip, rects 64x64 -> 64x64, the strip read by nothing else (exit %d)' % r.returncode)


WIRING = [
    ('switch table row (PerFrame, default 0)', 'src/renderer_adapter/rdr_switches.h', 'X(PrelightLayered, "wwe2k14_rdr_prelight_layered", PerFrame, 0)'),
    ('cvar defined', 'src/renderer_adapter/reader.h', 'REXCVAR_DEFINE_BOOL(wwe2k14_rdr_prelight_layered, false'),
    ('read every frame with the guest frame (BoolSeen)', 'src/renderer_adapter/binder.h', 'rdr_sw::BoolSeen(rdr_sw::kPrelightLayered, prelight_layered::g_fview.load(std::memory_order_relaxed), frame.header.guest_frame)'),
    ('the planner runs before the alias plan', 'src/renderer_adapter/binder.h', 'PreLightBegin(frame, stats);'),
    ('the group copies are an op another hook drops (alias op_excluded)', 'src/renderer_adapter/binder.h', 'pl_plan_.role[p] == 3'),
    ('the layered target goes through IssueTargets', 'src/renderer_adapter/binder.h', 'IssueTargets(frame, g.set_op, builder, stats, &layered);'),
    ('the layered program and the face viewport', 'src/renderer_adapter/binder.h', 'PreLightLayeredDraw(state, dynamic, ps,'),
    ('the owned hook must have drawn it (else the draw is not issued)', 'src/renderer_adapter/binder.h', 'draw_trace_.fate != kFateSmallPassesOurs'),
    ('the live census every 600 frames', 'src/renderer_adapter/binder.h', 'pl_enabled_ && ++pl_window_ >= 600'),
    ('the cube gets target usage when made', 'src/renderer_adapter/binder.h', 'desc.usage = uint8_t(desc.usage | rdr::kUsageTarget);'),
    ('F1: the post claim is read through PostClaimPlanned at plan time', 'src/renderer_adapter/binder.h', 'post_claim_.drop.data());\n    if (turns_on_ && turns_plan_.drop.size() >= frame.ops.count) ctx.others.push_back(turns_plan_.drop.data());\n    if (plane_blank_on_ && plane_blank_plan_.drop.size()'),
    ('F2: the device enables multiview only when the caller asked', 'src/renderer/device.cpp', 'options.multiview && multiview_available.multiview'),
    ('F2: the start-time cvar reaches the device options', 'src/renderer_adapter/live.cpp', 'ro.multiview = options.prelight_layered;'),
    ('F3: the layered pass loads CLEAR', 'src/renderer/renderer.cpp', 'plan.load[c] = VK_ATTACHMENT_LOAD_OP_CLEAR;\n        const bool sentinel'),
    ('F4: refused until the draw reaches the builder', 'src/renderer_adapter/binder.h', 'pl_layered_ = true, pl_refused_ = true;'),
    ('F6: no in-pass occlusion query in a multiview pass', 'src/renderer/renderer.cpp', 'const bool occ_on = trace && !layered_pass'),
    ('F6: no in-pass statistics query in a multiview pass', 'src/renderer/renderer.cpp', 'if (ps_on && !layered_pass)'),
    ('F8: a malformed layered target is a Problem, drawn as one view', 'src/renderer/renderer.cpp', 'Problem("layered_target_bad"'),
    ('F8: game refs never carry a view count', 'src/renderer_adapter/binder.h', 'targets.depth.pad = 0;'),
    ('F9: one range barrier for the layers', 'src/renderer/renderer.cpp', 'sub_count};'),
    ('F9: PrebuildOne checks multiview before making a module', 'src/renderer/renderer.cpp', 'if (r.pass.pad > 1 && !device.caps().multiview)'),
    ('multiview is asked of the renderer', 'src/renderer_adapter/binder.h', 'ctx.multiview = renderer_.multiview_supported()'),
    ('the render pass carries the view mask', 'src/renderer/renderer.cpp', 'multiview.pViewMasks = &view_mask;'),
    ('the framebuffer view is a 2D array of the layers', 'src/renderer/renderer.cpp', 'VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D'),
    ('the layers are transitioned', 'src/renderer/renderer.cpp', ': ViewCount(targets.color[c]) /* S102 C1: a layered target'),
    ('the device enables the feature only when it has it', 'src/renderer/device.cpp', 'multiview_enabled.multiview = options.multiview && multiview_available.multiview;'),
]


def wiring(root):
    for what, rel, needle in WIRING:
        path = os.path.join(root, rel)
        try:
            text = open(path, encoding='utf-8', errors='replace').read().replace('\r\n', '\n')
        except OSError:
            line(False, 'wiring: cannot read %s' % rel)
            continue
        ok = needle in text
        planted = text.replace(needle, '', 1)
        line(ok, 'wiring: %s' % what)
        line(planted != text and needle not in planted, '  twin: with that link removed the check fails (did apply: %s)' % (planted != text))
    for what, script, extra in (('own_pass_helpers_lint (no bare builder.SetTargets / builder.Copy in the new code)', os.path.join('tools', 'shadow', 'own_pass_helpers_lint.py'), ['--root', root]),
                                ('lint_seen (every BoolSeen passes a guest frame)', os.path.join('tools', 'lint', 'lint_seen.py'), ['--root', root]),
                                ('win_macro_lint (no member named small / near / far / interface)', os.path.join('tools', 'lint', 'win_macro_lint.py'), ['--root', root]),
                                ('device_features_lint', os.path.join('tools', 'renderer', 'device_features_lint.py'), ['--root', root]),
                                ('spv_lint (F7: every variant this lane does not name keeps byte-equal SPIR-V)', os.path.join('tools', 'c1', 'spv_lint.py'), ['--root', root])):
        if not os.path.exists(os.path.join(root, script)):
            line(False, '%s: the script %s is missing' % (what, script))
            continue
        r = subprocess.run([sys.executable, '-I', os.path.join(root, script)] + extra, capture_output=True, text=True, timeout=300)
        line(r.returncode == 0, '%s passes (exit %d)' % (what, r.returncode))


def rplay_counts(env, name, extra):
    out = os.path.join(env.work, name + '.json')
    p = subprocess.run([env.rplay, '--capture', env.caps['bryan'], '--frames', '91', '--counts', out, '--small-passes-ours', '--copy-batch', '1'] + extra, capture_output=True, text=True, timeout=900, errors='replace')
    if p.returncode != 0:
        raise RuntimeError('rplay %s exit %d: %s' % (name, p.returncode, (p.stdout + p.stderr)[-600:]))
    tot = {'copies': 0, 'barriers': 0, 'pipeline_binds': 0, 'passes': 0}
    n = 0
    with open(out, encoding='utf-8') as f:
        for ln in f:
            ln = ln.strip().rstrip(',')
            if ln.startswith('{"frame"'):
                d = json.loads(ln)
                if d['frame'] >= 10:
                    tot['copies'] += d['total']['copies']
                    tot['barriers'] += d['total'].get('barriers', 0)
                    tot['pipeline_binds'] += d['total'].get('pipeline_binds', 0)
                    tot['passes'] += d.get('passes', 0)
                    n += 1
    return {k: v / max(1, n) for k, v in tot.items()}, p.stdout + p.stderr


class CaseArm:
    """fview --case ZIP --replay with the game's files: the whole bundle, the picture hash of every frame."""
    def __init__(self, env, zip_path, name, extra):
        hashes = os.path.join(env.work, name + '.csv')
        if os.path.exists(hashes):
            os.remove(hashes)
        cmd = [env.fview, '--case', zip_path, '--replay', '--frames', '1000', '--validation', '1', '--game', env.game, '--hash-frames', hashes] + BUNDLE + extra
        for _ in range(12):
            p = subprocess.run(cmd, capture_output=True, text=True, timeout=1700, errors='replace')
            self.text = (p.stdout or '') + (p.stderr or '')
            if 'Not enough graphics memory for the frame ring' not in self.text:
                break
            time.sleep(20)
        self.rc = p.returncode
        with open(os.path.join(env.work, name + '.log'), 'w', encoding='utf-8') as f:
            f.write(self.text)
        self.hashes = {}
        if os.path.isfile(hashes):
            for ln in open(hashes):
                ln = ln.strip()
                if ln:
                    a, b = ln.split(',')
                    self.hashes[a] = b
        v = re.findall(r'validation errors (\d+)', self.text)
        self.errors = int(v[-1]) if v else -1
        self.c, self.census_line = parse_census(self.text)

    def valid(self):
        return self.rc == 0 and self.errors == 0 and len(self.hashes) > 1

    def differing(self, other):
        return sum(1 for k in self.hashes if k != 'format' and self.hashes.get(k) != other.hashes.get(k))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--only', default='census,wiring,replay,match,counts,trace,plants')
    ap.add_argument('--captures', default='bryan,brock')
    a = ap.parse_args()
    only = set(a.only.split(','))
    env = Env(os.path.abspath(a.root))
    for what, path in (('fview.exe', env.fview), ('rplay.exe', env.rplay)):
        if not os.path.exists(path):
            line(False, 'no %s (%s): build it with tools\\fview\\build.ps1 / tools\\renderer\\build.ps1' % (what, path))
            return 1
    for c in a.captures.split(','):
        if not os.path.isdir(env.caps[c]):
            line(False, 'cannot read capture %s (%s)' % (c, env.caps[c]))
            return 1
    if 'census' in only:
        census_check()
    if 'wiring' in only:
        wiring(env.root)
    bryan = {}
    if 'replay' in only or 'plants' in only or 'counts' in only:
        for cap in a.captures.split(','):
            off = Arm(env, cap, 'off', [])
            on = Arm(env, cap, 'on', ['--prelight-layered'])
            if cap == 'bryan':
                bryan['off'], bryan['on'] = off, on
            if 'replay' not in only:
                continue
            clean(off, '%s off' % cap)
            clean(on, '%s on' % cap)
            equal(off, on, '%s: switch on vs off (byte-equal, ours-vs-ours)' % cap)
            c = on.c
            line(on.census_line and c['groups'] > 0 and c['taken'] == c['groups'] and c['declined'] == 0 and c['fallback'] == 0 and c['with_reader'] == 0 and c['overlapping'] == 0,
                 '%s: census groups %d, taken %d, declined %d, fell back %d, taken with a reader %d, overlapping %d (frames with strips %d, taken %d)' % (cap, c['groups'], c['taken'], c['declined'], c['fallback'], c['with_reader'], c['overlapping'], c['with_strips'], c['frames_taken']))
            line(c['layered'] == c['taken'] and c['unmade'] == c['taken'] and c['dropped'] == 6 * c['taken'],
                 '%s: layered passes %d and strip passes not made %d = %d taken groups; copies dropped %d = 6 x taken' % (cap, c['layered'], c['unmade'], c['taken'], c['dropped']))
            fall = off.copies - on.copies
            per_frame = c['dropped'] / max(1, len(on.rows))
            line(abs(fall - per_frame) < 0.01 and fall > 0, '%s: copies a frame %.2f -> %.2f (-%.2f = the census %.2f)' % (cap, off.copies, on.copies, fall, per_frame))
            line(abs(off.targets - on.targets) < 1e-9, '%s: render passes (targets) a frame UNCHANGED: %.2f -> %.2f; draws %.2f -> %.2f' % (cap, off.targets, on.targets, off.draws, on.draws))
            # --copy-alias 2
            a_off = Arm(env, cap, 'alias2_off', ['--copy-alias', '2'])
            a_on = Arm(env, cap, 'alias2_on', ['--copy-alias', '2', '--prelight-layered'])
            clean(a_on, '%s copy-alias 2 on' % cap)
            equal(a_off, a_on, '%s: --copy-alias 2, switch on vs off' % cap)
            equal(on, a_on, '%s: --copy-alias 2 on equals the in-place run' % cap)
            line(a_on.c['taken'] == c['taken'], '%s: --copy-alias 2 takes the same groups (%d vs %d)' % (cap, a_on.c['taken'], c['taken']))
            # the shipped default bundle
            bundle = ['--shadows-ours', '--shadow-casters-ours', '--plane-shadow-ours']
            b_off = Arm(env, cap, 'bundle_off', bundle)
            b_on = Arm(env, cap, 'bundle_on', bundle + ['--prelight-layered'])
            clean(b_on, '%s default bundle on' % cap)
            equal(b_off, b_on, '%s: default bundle, switch on vs off' % cap)
            line(b_on.c['taken'] > 0 and b_on.c['frames_taken'] > 0, '%s: under the default bundle the lever TOOK %d groups in %d of %d frames (declined %d)' % (cap, b_on.c['taken'], b_on.c['frames_taken'], len(b_on.rows), b_on.c['declined']))
    if 'replay' in only and 'bryan' in a.captures.split(','):
        off, on = bryan['off'], bryan['on']
        h_off = Arm(env, 'bryan', 'hook_off', ['--prelight-layered'], base=('--copy-batch', 'on'))
        h_ref = Arm(env, 'bryan', 'hook_off_ref', [], base=('--copy-batch', 'on'))
        clean(h_off, 'bryan hook off')
        line(h_off.c['taken'] == 0 and h_off.c['groups'] > 0 and h_off.c['dropped'] == 0 and 'small_passes_ours is off' in h_off.c['why'], 'bryan: without small_passes_ours every group is declined (%d of %d: %s)' % (h_off.c['declined'], h_off.c['groups'], h_off.c['why']))
        equal(h_ref, h_off, 'bryan hook off: equal to the stream, copies %.2f vs %.2f' % (h_ref.copies, h_off.copies))
        line(h_ref.copies == h_off.copies, 'bryan hook off: copies unchanged')
        nm = Arm(env, 'bryan', 'nomv', ['--prelight-layered', '--no-multiview'])
        clean(nm, 'bryan no multiview')
        line(nm.c['taken'] == 0 and nm.c['groups'] > 0 and 'no multiview' in nm.c['why'] and nm.c['dropped'] == 0, 'bryan --no-multiview: every group declined for it (%d of %d: %s), nothing dropped' % (nm.c['declined'], nm.c['groups'], nm.c['why']))
        equal(off, nm, 'bryan --no-multiview: equal to the stream path, copies %.2f vs %.2f' % (off.copies, nm.copies))
        line(abs(off.copies - nm.copies) < 1e-9 and abs(off.targets - nm.targets) < 1e-9, 'bryan --no-multiview: copies and passes unchanged (the fallback takes the stream path)')
        fb = Arm(env, 'bryan', 'fallback', ['--prelight-layered', '--replace-fault', 'small.cube_last_vertex'])
        fb_ref = Arm(env, 'bryan', 'fallback_ref', ['--replace-fault', 'small.cube_last_vertex'])
        clean(fb, 'bryan fallback')
        line(fb.c['fallback'] == fb.c['taken'] > 0, 'bryan fallback: the owned hook refuses every pre-light draw: %d groups fell back after the fact (of %d taken)' % (fb.c['fallback'], fb.c['taken']))
        equal(fb_ref, fb, 'bryan fallback: equal to the stream path')
        line(abs(fb.copies - fb_ref.copies) < 0.01, 'bryan fallback: copies unchanged (%.2f vs %.2f): the stream keeps its copies' % (fb.copies, fb_ref.copies))
        dead_base = ['--dead-work', '2', '--dead-work-fault', 'cut_live']
        d_off = Arm(env, 'bryan', 'dead_off', dead_base)
        d_on = Arm(env, 'bryan', 'dead_on', dead_base + ['--prelight-layered'])
        clean(d_on, 'bryan dead cut')
        equal(d_off, d_on, 'bryan dead cut: switch on vs off')
        line('dead skip' in d_on.c['why'] and d_on.c['taken'] > 0 and d_on.c['overlapping'] == 0, 'bryan dead cut: G4 cut slots declined (%s), the others taken (%d), no op both cut and taken (%d)' % (d_on.c['why'], d_on.c['taken'], d_on.c['overlapping']))
    if 'match' in only:
        zip_path = os.path.join(env.main, MATCH_CASE)
        if not os.path.exists(zip_path) or not os.path.isdir(env.game):
            line(False, 'match: the case zip or the game files cannot be read (%s)' % zip_path)
        else:
            m_off = CaseArm(env, zip_path, 'match_off', [])
            m_on = CaseArm(env, zip_path, 'match_on', ['--prelight-layered'])
            line(m_off.valid() and m_on.valid(), 'match: validation clean both arms (error counts %d / %d, exit codes %d / %d, %d frames)' % (m_off.errors, m_on.errors, m_off.rc, m_on.rc, len(m_on.hashes) - 1))
            line(m_off.differing(m_on) == 0 and len(m_off.hashes) == len(m_on.hashes), 'match: switch on vs off, %d frames differ of %d (the match bakes no pre-light cube: groups %d, taken %d)' % (m_off.differing(m_on), len(m_on.hashes) - 1, m_on.c['groups'], m_on.c['taken']))
    if 'counts' in only and bryan:
        c_off, _ = rplay_counts(env, 'r_off', [])
        c_on, t1 = rplay_counts(env, 'r_on', ['--prelight-layered'])
        print('      renderer counts a frame (Bryan, frames >= 10, G12 copy batch ON in both arms): off %s' % {k: round(v, 2) for k, v in c_off.items()})
        print('      renderer counts a frame (Bryan, frames >= 10, G12 copy batch ON in both arms): on  %s' % {k: round(v, 2) for k, v in c_on.items()})
        line(c_off['copies'] - c_on['copies'] >= 4.99, 'rplay: copies %.2f -> %.2f a frame (-%.2f: the 5 batched copy commands of the strips are gone)' % (c_off['copies'], c_on['copies'], c_off['copies'] - c_on['copies']))
        line(abs(c_on['passes'] - c_off['passes']) < 1e-9, 'rplay: passes a frame unchanged (%.2f -> %.2f)' % (c_off['passes'], c_on['passes']))
        line(c_on['barriers'] < c_off['barriers'], 'rplay: barriers a frame fall (%.2f -> %.2f)' % (c_off['barriers'], c_on['barriers']))
        line(c_on['pipeline_binds'] <= c_off['pipeline_binds'], 'rplay: pipeline binds a frame do not rise (%.2f -> %.2f)' % (c_off['pipeline_binds'], c_on['pipeline_binds']))
        c_nb_off, _ = rplay_counts(env, 'r_nb_off', ['--copy-batch', '0'])
        c_nb_on, _ = rplay_counts(env, 'r_nb_on', ['--copy-batch', '0', '--prelight-layered'])
        line(c_nb_off['copies'] - c_nb_on['copies'] >= 29.99, 'rplay, copy batch OFF in both arms: copies %.2f -> %.2f a frame (-%.2f: the 30 a frame are gone)' % (c_nb_off['copies'], c_nb_on['copies'], c_nb_off['copies'] - c_nb_on['copies']))
    if 'trace' in only or 'counts' in only:
        # F6 (C1 review): in-pass queries (G10 statistics, G11 per-draw occlusion) inside a multiview pass: rplay --frame-trace + --draw-census 2 with the switch on, validation 0, exit 0
        tfile = os.path.join(env.work, 'frame_trace.jsonl')
        p = subprocess.run([env.rplay, '--capture', env.caps['bryan'], '--frames', '40', '--validation', '1', '--small-passes-ours', '--prelight-layered', '--frame-trace', tfile, '--gpu-census', '1', '--gpu-stamps', '1',
                            '--draw-census', '2'], capture_output=True, text=True, timeout=900, errors='replace')
        txt = (p.stdout or '') + (p.stderr or '')
        cen, got = parse_census(txt)
        traced = os.path.exists(tfile) and os.path.getsize(tfile) > 0
        line(p.returncode == 0 and traced and got and cen['taken'] > 0, 'F6: rplay --frame-trace + --draw-census 2 + gpu stamps with the layered pass on: exit code %d (rplay exits 4 on validation errors), trace file written %s, layered groups taken %d' % (p.returncode, traced, cen['taken']))
    if 'plants' in only and bryan:
        off, on = bryan['off'], bryan['on']

        def plant(name, extra):
            return Arm(env, 'bryan', 'plant_' + name, extra)
        for name in ('swap_faces', 'missing_layer'):
            p = plant(name, ['--prelight-layered', '--prelight-layered-fault', name])
            pic, cube, _ = off.diff(p)
            line(p.rc in (0, 4) and (pic > 0 or cube > 0) and p.c['taken'] == on.c['taken'], 'twin %s is CAUGHT: %d pictures and %d cube hashes differ of %d (taken %d)' % (name, pic, cube, len(off.rows), p.c['taken']))
        p = plant('keep_copies', ['--prelight-layered', '--prelight-layered-fault', 'keep_copies'])
        pic, cube, _ = off.diff(p)
        line(abs(p.copies - off.copies) < 0.01 and p.c['taken'] > 0, 'twin keep_copies is CAUGHT by the counts: copies %.2f a frame, the clean arm has %.2f (off %.2f); cube hashes differ on %d frames (the copies read a strip nobody made)' % (p.copies, on.copies, off.copies, cube))
        p = plant('fallback_no_strip', ['--prelight-layered', '--no-multiview', '--prelight-layered-fault', 'fallback_no_strip'])
        pic, cube, _ = off.diff(p)
        line(p.rc in (0, 4) and (pic > 0 or cube > 0) and p.c['taken'] == 0, 'twin fallback_no_strip is CAUGHT: the fallback taken (taken %d) but the strip not made: %d pictures and %d cube hashes differ of %d' % (p.c['taken'], pic, cube, len(off.rows)))
        # ---- review fixes: F3 sentinel + scissor, F4 no_geometry, F5 clip transform, F8 bad pad, renderer plants
        s = plant('sentinel', ['--prelight-layered', '--layered-clear-sentinel'])
        clean(s, 'bryan sentinel (the layered pass clears magenta)')
        equal(off, s, 'bryan sentinel arm equals the normal off arm: every texel of every face is written by the pass (F3)')
        line(s.c['taken'] == on.c['taken'], 'bryan sentinel arm takes the same groups (%d)' % s.c['taken'])
        p = plant('shrink_scissor', ['--prelight-layered', '--prelight-layered-fault', 'shrink_scissor'])
        pic, cube, _ = off.diff(p)
        line(p.rc in (0, 4) and cube > 0 and p.c['taken'] == on.c['taken'], 'twin shrink_scissor is CAUGHT: the face scissor one texel smaller leaves a row / column unwritten: %d cube hashes differ of %d (F3)' % (cube, len(off.rows)))
        p = plant('no_geometry', ['--prelight-layered', '--prelight-layered-fault', 'no_geometry'])
        clean(p, 'bryan no_geometry (the layered draw never reaches the builder)')
        equal(off, p, 'bryan no_geometry: every group falls back after the fact and the pictures and cubes equal the stream (F4)')
        line(p.c['fallback'] > 0 and p.c['fallback'] == p.c['taken'] and abs(p.copies - off.copies) < 0.01, 'bryan no_geometry: fell back %d of %d taken, copies unchanged (%.2f vs %.2f)' % (p.c['fallback'], p.c['taken'], p.copies, off.copies))
        p = plant('ndc_offset_unscaled', ['--prelight-layered', '--prelight-layered-fault', 'ndc_offset_unscaled'])
        pic, cube, _ = off.diff(p)
        line(p.rc in (0, 4) and cube > 0, 'twin ndc_offset_unscaled is CAUGHT (F5: the x offset not multiplied by 6): %d cube hashes and %d pictures differ of %d' % (cube, pic, len(off.rows)))
        p = plant('half_viewport', ['--prelight-layered', '--prelight-layered-fault', 'half_viewport'])
        clean(p, 'bryan half_viewport')
        line(p.c['taken'] == 0 and p.c['groups'] > 0 and 'clip transform' in p.c['why'], 'twin half_viewport is DECLINED by the clip-transform rule (F5): taken %d of %d (%s)' % (p.c['taken'], p.c['groups'], p.c['why']))
        equal(off, p, 'bryan half_viewport: declined groups equal the stream path')
        p = plant('bad_pad', ['--prelight-layered-fault', 'bad_pad'])   # the DEV exe: a Problem aborts by design (problem.cpp): the Problem must be raised, with its name, before the abort
        line('layered_target_bad' in p.text and 'a layered colour beside a depth target' in p.text, 'planted pad = 6 on a one-layer image (F8), dev exe: the renderer raises the Problem layered_target_bad (a dev build aborts on any Problem: exit %d)' % p.rc)
        if not os.path.exists(env.fview_rel):
            line(False, 'no release fview.exe (%s): build it with tools\\fview\\build.ps1 -Release' % env.fview_rel)
        else:
            q = Arm(env, 'bryan', 'plant_bad_pad_rel', ['--prelight-layered-fault', 'bad_pad'], exe=env.fview_rel)
            line(q.rc in (0,) and 'layered_target_bad' in q.text and q.errors == 0 and len(q.rows) == len(off.rows), 'planted pad = 6 on a one-layer image (F8), player build: the Problem is logged, no crash (exit %d), validation %d, %d frames' % (q.rc, q.errors, len(q.rows)))
            equal(off, q, 'bryan bad_pad, player build: the malformed target is drawn as an ordinary one-view pass (pictures and cubes equal)')
        p = plant('view_mask', ['--prelight-layered', '--layered-plant', '1'])
        pic, cube, _ = off.diff(p)
        line(p.rc in (0, 4) and cube > 0, 'renderer plant 1 (view mask (1 << N) - 2: view 0 never drawn) is CAUGHT: %d cube hashes differ of %d' % (cube, len(off.rows)))
        p = plant('transition_one_layer', ['--prelight-layered', '--layered-plant', '2'])
        line(p.rc == 4 or p.errors > 0, 'renderer plant 2 (the layered transition covers one layer only) is CAUGHT BY VALIDATION: error count %d, exit code %d' % (p.errors, p.rc))
        p = plant('phantom_reader', ['--prelight-layered', '--prelight-layered-fault', 'phantom_reader'])
        line(p.c['declined'] > 0 and 'read by something else' in p.c['why'], 'phantom_reader alone: the strip scan declines the read strip (%s)' % p.c['why'])
        p = plant('no_strip_check', ['--prelight-layered', '--prelight-layered-fault', 'phantom_reader,no_strip_check'])
        line(p.c['with_reader'] > 0, 'twin no_strip_check (a strip reader left unchecked) is CAUGHT: %d groups taken although the strip is read (the clean arm: %d)' % (p.c['with_reader'], on.c['with_reader']))
        p = plant('ignore_overlap', ['--prelight-layered', '--prelight-layered-fault', 'ignore_overlap', '--dead-work', '2', '--dead-work-fault', 'cut_live'])
        line(p.c['overlapping'] > 0, 'twin ignore_overlap is CAUGHT: %d groups taken with an op in G4\'s drop set (the clean arm: 0)' % p.c['overlapping'])
    print('%s: %d failure(s)' % ('FAIL' if FAILS else 'ok', FAILS))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
