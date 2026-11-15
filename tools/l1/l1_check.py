r"""S102 lane L1 offline proof, 0 game runs, 0 Surface runs: the pre-light cube bake is not drawn; the character shaders evaluate its four lights per pixel
(src\renderer_adapter\prelight_analytic.h, switch wwe2k14_rdr_prelight_analytic; shaders fx_prelight.glsl, the `<name>_pl` twins of tools\renderer\compile_shaders.py).
  py -I tools\l1\l1_check.py [--root DIR] [--only census,wiring,replay,pics,counts,plants,match] [--captures bryan,brock]

The picture of the analytic path is NOT byte-equal to the cube path (the cube is 64x64 per face, 8-bit, bilinear; the analytic path is exact), so the bar is a histogram measured on the wrestler pixels
(the pixels a `--draw-drop` of the wrestlers changes; the cube only ever lights wrestlers): mean, 99.9th percentile, the share of pixels over 12 levels, and ZERO differing pixels outside the wrestlers'
silhouette beyond a handful (the silhouette mask itself is a few pixels short of the shaded edge). Arms (every one `--small-passes-ours --copy-batch on --validation 1`):
  main      off vs on, every frame of Bryan and every 4th of Check B: validation 0 (error count AND exit code); census: groups taken, readers twinned = readers planned, none missed; ops / targets / draws / copies
            a frame fall by exactly 8 / 1 / 1 / 6 per taken group (the frame the renderer is given: FVIEW_SWEEP_COUNTS); the histogram bars.
  alias2    --copy-alias 2 on: the picture hashes equal the in-place run's, the same groups taken.
  bundle    the shipped default bundle (--shadows-ours --shadow-casters-ours --plane-shadow-ours): the lever TOOK groups in most frames, histogram bars hold.
  dead      --dead-work 2 --dead-work-fault cut_live (G4's pre-light cut made real): the cut slots are declined as dead, no op both cut and taken.
  layered   --prelight-layered too: this lever's groups are not C1's (declined there as another planner's), pictures equal the analytic-only arm.
  quantise  informational: the shader rounds the result to 8 bits as the cube stored it (the mean does not fall: the 8-bit storage is not the difference).
counts: rplay renderer counts a frame (Bryan, G12 copy batch ON in both arms): passes, barriers, pipeline binds, copies, draws do not rise; copies and passes fall.
Planted twins (each must be CAUGHT): light_missing, wrong_space, alpha_dropped, stale_k (the first frame's constants kept: histogram over the bar), keep_bake (the counts do not fall), phantom_reader (declined: a reader without a twin),
phantom_reader + no_reader_check (taken with an untwinned reader > 0), ignore_overlap + the dead cut (taken overlapping > 0), no_twin (the readers keep the cube nobody baked: missed readers, pictures).
Also: the wiring with each link removed in a text copy, the lints (own_pass_helpers_lint, lint_seen, win_macro_lint, spv_lint), the capture census (tools\l1\l1_census.py). Exit 1 on any FAIL; a missing
capture or exe FAILS."""
import argparse
import csv
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
import numpy as np  # noqa: E402
import l1_pics  # noqa: E402

FAILS = 0
BUNDLE = ['--shadows-ours', '--shadow-casters-ours', '--plane-shadow-ours']
# The bars (measured on Bryan + Check B, see docs\renderer\PRELIGHT_DIRECT.md section 8.4: the clean arm sits well under every one; each planted twin goes far over at least one)
BAR_MEAN = 1.0          # mean absolute difference over the wrestler pixels, levels of 255 per channel
BAR_P999 = 8            # the 99.9th percentile of the per-pixel maximum channel difference
BAR_GT12 = 0.0015       # the share of wrestler pixels over 12 levels
BAR_OUTSIDE = 400       # differing pixels outside the silhouette mask (the mask is the draw-drop difference: a few pixels at its edge)
PLANT_FACTOR = 3.0      # a planted twin's mean must exceed the bar by this factor (or its p99.9 / share bar)


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
        self.rplay = os.path.join(root, 'out', 'renderer', 'dev', 'rplay.exe')
        self.work = os.path.join(root, 'out', 'l1')
        os.makedirs(self.work, exist_ok=True)
        self.caps = {'bryan': os.path.join(self.main, 'out', 'lane86', 'bryan', 'capture'), 'brock': os.path.join(self.main, 'out', 'lane86', 'brock', 'capture')}


CENSUS = re.compile(r'\[rdr\]\[prelight_analytic\] frames (\d+) \(with strips (\d+), taken (\d+)\), groups (\d+), taken (\d+), declined (\d+)(?: \((.*?)\))? \| readers planned (\d+), twinned (\d+), missed (\d+) \| '
                    r'bake draws dropped (\d+), copies dropped (\d+) \| audit: taken with an untwinned reader (\d+), taken overlapping (\d+) \| orphan readers (\d+), stale reads (\d+), taken with the ops already dropped (\d+)')
KEYS = ('frames', 'with_strips', 'frames_taken', 'groups', 'taken', 'declined', 'why', 'planned', 'twinned', 'missed', 'draws_dropped', 'copies_dropped', 'untwinned', 'overlapping', 'orphan', 'stale', 'elsewhere')


def parse_census(text):
    m = CENSUS.search(text)
    return dict(zip(KEYS, [(x or '') if i == 6 else int(x or 0) for i, x in enumerate(m.groups())])) if m else dict.fromkeys(KEYS, 0), bool(m)


class Arm:
    """fview --sweep: the picture hash of every frame and the frame the renderer is given (counts), validation read from the error count AND the exit code."""
    def __init__(self, env, cap, name, extra, base=('--small-passes-ours', '--copy-batch', 'on')):
        out = os.path.join(env.work, '%s_%s.csv' % (cap, name))
        if os.path.exists(out):
            os.remove(out)
        e = dict(os.environ, FVIEW_SWEEP_COUNTS='1')
        cmd = [env.fview, '--capture', env.caps[cap], '--validation', '1', '--sweep', out, '--sweep-slot', '13'] + list(base) + list(extra)
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=1800, env=e, errors='replace')
        self.text = (p.stdout or '') + (p.stderr or '')
        with open(os.path.join(env.work, '%s_%s.log' % (cap, name)), 'w', encoding='utf-8') as f:
            f.write(self.text)
        self.rc = p.returncode
        v = re.findall(r'validation errors (\d+)', self.text)
        self.errors = int(v[-1]) if v else -1
        self.rows = list(csv.DictReader(open(out))) if os.path.exists(out) else []
        n = max(1, len(self.rows))
        for k in ('ops', 'targets', 'clears', 'draws', 'copies'):
            setattr(self, k, sum(int(r['gpu_' + k]) for r in self.rows) / n)
        self.c, self.census_line = parse_census(self.text)

    def valid(self):
        return self.rc == 0 and self.errors == 0 and bool(self.rows)

    def pics_equal(self, other):
        n = min(len(self.rows), len(other.rows))
        return sum(1 for i in range(n) if self.rows[i]['picture'] != other.rows[i]['picture']), len(self.rows) != len(other.rows)


def clean(a, text):
    line(a.valid(), '%s: validation clean (error count %d, exit code %d, %d frames)' % (text, a.errors, a.rc, len(a.rows)))


def stats_for(env, cap, frames, extra, off_pics, drop_pics, tag):
    """the histogram of an arm against the off arm, over the wrestler pixels (mask = off vs draw-drop)"""
    on, text = l1_pics.render_arm(env.fview, env.caps[cap], frames, os.path.join(env.work, 'pics_%s_%s' % (cap, tag)), ['--copy-batch', 'on'] + extra)
    out = {}
    for fr in frames:
        mask = np.abs(off_pics[fr] - drop_pics[fr]).max(axis=2) > 0
        out[fr] = l1_pics.diff_stats(off_pics[fr], on[fr], mask)
    return out, text


def summarise(per_frame):
    px = sum(s['pixels'] for s in per_frame.values())
    return dict(mean=sum(s['mean'] * s['pixels'] for s in per_frame.values()) / max(1, px), p999=max(s['p999'] for s in per_frame.values()), gt12=sum(s['gt12'] * s['pixels'] for s in per_frame.values()) / max(1, px),
                outside=max(s['outside'] for s in per_frame.values()), px=px, mx=max(s['max'] for s in per_frame.values()))


def within_bars(s):
    return s['mean'] <= BAR_MEAN and s['p999'] <= BAR_P999 and s['gt12'] <= BAR_GT12 and s['outside'] <= BAR_OUTSIDE and s['px'] > 1000


def census_check(env):
    r = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'l1_census.py'), env.caps['bryan'], '20', '60', '85'], capture_output=True, text=True, timeout=900)
    print('\n'.join('      ' + ln for ln in r.stdout.strip().splitlines()[:6]))
    ok = r.returncode == 0 and '5 pre-light draws' in r.stdout and r.stdout.count(' read by yCh_') >= 3
    line(ok, 'the captures\' pre-light groups and the character draws that read their cubes (tools\\l1\\l1_census.py, exit %d)' % r.returncode)


WIRING = [
    ('switch table row (PerFrame, default 0)', 'src/renderer_adapter/rdr_switches.h', 'X(PrelightAnalytic, "wwe2k14_rdr_prelight_analytic", PerFrame, 0)'),
    ('cvar defined', 'src/renderer_adapter/reader.h', 'REXCVAR_DEFINE_BOOL(wwe2k14_rdr_prelight_analytic, false'),
    ('read every frame with the guest frame (BoolSeen)', 'src/renderer_adapter/binder.h', 'rdr_sw::BoolSeen(rdr_sw::kPrelightAnalytic, prelight_analytic::g_fview.load(std::memory_order_relaxed), frame.header.guest_frame)'),
    ('the planner runs before C1\'s and before the alias plan', 'src/renderer_adapter/binder.h', 'PreLightAnalyticBegin(frame, stats);'),
    ('the walk leaves the taken groups\' ops out', 'src/renderer_adapter/binder.h', 'if (pa_on_ && pa_plan_.drop[op_index]) {'),
    ('the group copies are an op another hook drops (alias op_excluded)', 'src/renderer_adapter/binder.h', 'if (pa_on_ && pa_plan_.drop[p]) return true;'),
    ('C1 sees this drop set (decline on overlap)', 'src/renderer_adapter/binder.h', 'if (pa_on_ && pa_plan_.drop.size() >= frame.ops.count) ctx.others.push_back(pa_plan_.drop.data());'),
    ('the reader\'s program is the twin', 'src/renderer_adapter/binder.h', 'state.program = Program(vs, twin, fixed.alpha_test != 0 && !drop_alpha_test);'),
    ('the reader\'s cube is not bound', 'src/renderer_adapter/binder.h', 'textures.slots[prelight_analytic::kReaderSlot] = rdr::TextureSlot{};'),
    ('the reader\'s constants carry the bake\'s (PreLightK)', 'src/renderer_adapter/binder.h', ': pa_k ? PackAnalytic(rdr::fx::kVariants[pa_ps], frame.constant_banks.Ref(draw.ps_bank), bools, builder, pa_k)'),
    ('the group\'s constants come from the pre-light draw\'s own bank', 'src/renderer_adapter/binder.h', 'bank.CopyBytes(204 * 16, 3 * 16, out + 11 * 16);    // SssLobeA'),
    ('a reader without a twin keeps the bake (kind 2)', 'src/renderer_adapter/binder.h', 'return slot == prelight_analytic::kReaderSlot && pa_twin_available(ps) ? 1 : 2;'),
    ('the live census every 600 frames', 'src/renderer_adapter/binder.h', 'pa_enabled_ && ++pa_window_ >= 600'),
    ('the shader samples the lights, not the cube, under the define (skin)', 'src/renderer/shaders/fx/fx_skin.glsl', 'vec4 cubeLight = fx_prelight_eval(n);'),
    ('the shader samples the lights, not the cube, under the define (gear)', 'src/renderer/shaders/fx/fx_gear.glsl', 'fx_prelight_eval(n).rgb'),
    ('the shader samples the lights, not the cube, under the define (trans)', 'src/renderer/shaders/fx/fx_trans_ps.glsl', 'fx_prelight_eval(n).xyz'),
    ('the twins are made by the shader compiler', 'tools/renderer/compile_shaders.py', "'defines': list(v.get('defines', [])) + ['FX_PRELIGHT_ANALYTIC']"),
    ('rplay arm kind 5 toggles the switch per frame', 'src/renderer/player/rplay.cpp', 'prelight_analytic::g_fview.store(g_arms[arm].names == "1");'),
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
                                ('lint_seen (every BoolSeen passes a guest frame)', os.path.join('tools', 'gate', 'lint_seen.py'), ['--root', root]),
                                ('win_macro_lint (no member named small / near / far / interface)', os.path.join('tools', 'gate', 'win_macro_lint.py'), ['--root', root]),
                                ('spv_lint (every variant this lane does not name keeps byte-equal SPIR-V)', os.path.join('tools', 'l1', 'spv_lint.py'), ['--root', root])):
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
    tot = {'copies': 0, 'barriers': 0, 'pipeline_binds': 0, 'passes': 0, 'draws': 0}
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
                    tot['draws'] += d['total'].get('draws', 0)
                    tot['passes'] += d.get('passes', 0)
                    n += 1
    return {k: v / max(1, n) for k, v in tot.items()}, p.stdout + p.stderr


def frames_of(cap, n):
    return list(range(0, n, 1)) if cap == 'bryan' else list(range(0, n, 4))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--only', default='census,wiring,replay,pics,review,counts,plants')
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
        census_check(env)
    if 'wiring' in only:
        wiring(env.root)
    arms = {}
    if 'replay' in only or 'plants' in only or 'pics' in only or 'review' in only:
        for cap in a.captures.split(','):
            off = Arm(env, cap, 'off', [])
            on = Arm(env, cap, 'on', ['--prelight-analytic'])
            arms[cap] = {'off': off, 'on': on}
            if 'replay' not in only:
                continue
            clean(off, '%s off' % cap)
            clean(on, '%s on' % cap)
            c = on.c
            line(on.census_line and c['groups'] > 0 and c['taken'] > 0 and c['planned'] == c['twinned'] and c['missed'] == 0 and c['untwinned'] == 0 and c['overlapping'] == 0,
                 '%s: census groups %d, taken %d, declined %d (%s), readers planned %d twinned %d missed %d, taken with an untwinned reader %d, overlapping %d (frames with strips %d, taken %d)' % (
                     cap, c['groups'], c['taken'], c['declined'], c['why'] or 'none', c['planned'], c['twinned'], c['missed'], c['untwinned'], c['overlapping'], c['with_strips'], c['frames_taken']))
            n = max(1, len(on.rows))
            per = c['taken'] / n
            line(abs((off.ops - on.ops) - 8 * per) < 0.01 and abs((off.targets - on.targets) - per) < 0.01 and abs((off.draws - on.draws) - per) < 0.01 and abs((off.copies - on.copies) - 6 * per) < 0.01,
                 '%s: the frame the renderer is given a frame: ops %.2f -> %.2f, targets %.2f -> %.2f, draws %.2f -> %.2f, copies %.2f -> %.2f = 8 / 1 / 1 / 6 per taken group (%.2f groups a frame)' % (
                     cap, off.ops, on.ops, off.targets, on.targets, off.draws, on.draws, off.copies, on.copies, per))
            line(c['draws_dropped'] == c['taken'] and c['copies_dropped'] == 6 * c['taken'], '%s: bake draws dropped %d = taken, copies dropped %d = 6 x taken' % (cap, c['draws_dropped'], c['copies_dropped']))
            # --copy-alias 2
            a_on = Arm(env, cap, 'alias2_on', ['--copy-alias', '2', '--prelight-analytic'])
            clean(a_on, '%s copy-alias 2 on' % cap)
            ne, mism = on.pics_equal(a_on)
            line(ne == 0 and not mism and a_on.c['taken'] == c['taken'], '%s: --copy-alias 2 on equals the in-place run (%d of %d pictures differ), the same groups taken (%d vs %d)' % (cap, ne, len(on.rows), a_on.c['taken'], c['taken']))
            # the shipped default bundle
            b_on = Arm(env, cap, 'bundle_on', BUNDLE + ['--prelight-analytic'])
            clean(b_on, '%s default bundle on' % cap)
            line(b_on.c['taken'] > 0 and b_on.c['frames_taken'] > 0.8 * len(b_on.rows), '%s: under the default bundle the lever TOOK %d groups in %d of %d frames (declined %d: %s)' % (cap, b_on.c['taken'], b_on.c['frames_taken'], len(b_on.rows), b_on.c['declined'], b_on.c['why'] or 'none'))
            # C1 too: this lever first
            l_on = Arm(env, cap, 'layered_on', ['--prelight-analytic', '--prelight-layered'])
            clean(l_on, '%s analytic + layered' % cap)
            ne, mism = on.pics_equal(l_on)
            line(ne == 0 and not mism and l_on.c['taken'] == c['taken'], '%s: with --prelight-layered too the analytic groups are not C1\'s (pictures equal the analytic-only arm: %d differ; taken %d)' % (cap, ne, l_on.c['taken']))
    if 'replay' in only and 'bryan' in arms:
        dead_base = ['--dead-work', '2', '--dead-work-fault', 'cut_live']
        d_on = Arm(env, 'bryan', 'dead_on', dead_base + ['--prelight-analytic'])
        clean(d_on, 'bryan dead cut + analytic')
        line(d_on.c['elsewhere'] > 0 and d_on.c['overlapping'] == 0 and d_on.c['stale'] == 0 and 'dead skip' not in d_on.c['why'], 'bryan dead cut: the slots G4 cut (all 8 ops dropped elsewhere) are taken with fresh constants (%d), nothing dropped twice (overlapping %d), stale reads %d, taken %d of %d groups' % (
            d_on.c['elsewhere'], d_on.c['overlapping'], d_on.c['stale'], d_on.c['taken'], d_on.c['groups']))
        h_off = Arm(env, 'bryan', 'nohook', ['--prelight-analytic'], base=('--copy-batch', 'on'))
        clean(h_off, 'bryan without small_passes_ours')
        line(h_off.c['taken'] > 0, 'bryan without small_passes_ours the lever still takes groups (it needs no owned hook): taken %d' % h_off.c['taken'])
    if 'pics' in only or 'plants' in only:
        frames_by_cap = {}
        for cap in a.captures.split(','):
            n = len(arms[cap]['off'].rows) if cap in arms else 91
            frames_by_cap[cap] = frames_of(cap, n)
    if 'pics' in only:
        for cap in a.captures.split(','):
            frames = frames_by_cap[cap]
            off_pics, _ = l1_pics.render_arm(env.fview, env.caps[cap], frames, os.path.join(env.work, 'pics_%s_off' % cap), ['--copy-batch', 'on'])
            drop_pics, _ = l1_pics.render_arm(env.fview, env.caps[cap], frames, os.path.join(env.work, 'pics_%s_drop' % cap), ['--copy-batch', 'on', '--draw-drop', '@class=wrestlers'])
            arms[cap]['off_pics'], arms[cap]['drop_pics'] = off_pics, drop_pics
            per, _ = stats_for(env, cap, frames, ['--prelight-analytic'], off_pics, drop_pics, 'on')
            s = summarise(per)
            line(within_bars(s), '%s: %d frames, %d wrestler pixels: mean %.3f (bar %.1f), p99.9 %d (bar %d), over 12 levels %.4f%% (bar %.2f%%), max %d, outside the silhouette %d (bar %d)' % (
                cap, len(frames), s['px'], s['mean'], BAR_MEAN, s['p999'], BAR_P999, 100 * s['gt12'], 100 * BAR_GT12, s['mx'], s['outside'], BAR_OUTSIDE))
            arms[cap]['stats_on'] = s
            per_b, _ = stats_for(env, cap, frames[::3] or frames, BUNDLE + ['--prelight-analytic'], off_pics, drop_pics, 'bundle')
            # (the bundle's own off arm differs from the plain one: the bar is judged against the bundle's off arm)
            off_b, _ = l1_pics.render_arm(env.fview, env.caps[cap], frames[::3] or frames, os.path.join(env.work, 'pics_%s_off_bundle' % cap), ['--copy-batch', 'on'] + BUNDLE)
            drop_b, _ = l1_pics.render_arm(env.fview, env.caps[cap], frames[::3] or frames, os.path.join(env.work, 'pics_%s_drop_bundle' % cap), ['--copy-batch', 'on', '--draw-drop', '@class=wrestlers'] + BUNDLE)
            per_b, _ = stats_for(env, cap, frames[::3] or frames, BUNDLE + ['--prelight-analytic'], off_b, drop_b, 'bundle')
            sb = summarise(per_b)
            line(within_bars(sb), '%s: default bundle, %d frames: mean %.3f, p99.9 %d, over 12 levels %.4f%%, max %d, outside %d' % (cap, len(per_b), sb['mean'], sb['p999'], 100 * sb['gt12'], sb['mx'], sb['outside']))
            per_q, _ = stats_for(env, cap, frames[::3] or frames, ['--prelight-analytic', '--prelight-analytic-fault', 'quantise'], off_pics, drop_pics, 'quantise')
            sq = summarise({k: v for k, v in per_q.items()})
            ref = summarise({k: per[k] for k in per_q})
            print('      note: rounding the result to 8 bits as the cube stored it changes the mean difference %.3f -> %.3f (max %d -> %d): the 8-bit storage is not what separates the paths (the cube sampling is)' % (ref['mean'], sq['mean'], ref['mx'], sq['mx']))
    if 'review' in only and 'brock' in arms and 'bryan' in arms:
        def bars(cap, frames, off_extra, on_extra, tag):
            """the histogram of an on arm against an off arm that has the same other flags, over the wrestler pixels; the census of the on arm"""
            offp, _ = l1_pics.render_arm(env.fview, env.caps[cap], frames, os.path.join(env.work, 'rv_%s_off' % tag), ['--copy-batch', 'on'] + off_extra)
            dropp, _ = l1_pics.render_arm(env.fview, env.caps[cap], frames, os.path.join(env.work, 'rv_%s_drop' % tag), ['--copy-batch', 'on', '--draw-drop', '@class=wrestlers'] + off_extra)
            onp, text = l1_pics.render_arm(env.fview, env.caps[cap], frames, os.path.join(env.work, 'rv_%s_on' % tag), ['--copy-batch', 'on'] + on_extra)
            per = {f: l1_pics.diff_stats(offp[f], onp[f], np.abs(offp[f] - dropp[f]).max(axis=2) > 0) for f in frames}
            return summarise(per), parse_census(text)[0]
        # F1: G3's pre-light turn drops all 8 ops of a slot on alternate frames; the readers must not read a cube nobody baked
        s, c = bars('bryan', list(range(0, 91, 3)), ['--turns', '8'], ['--turns', '8', '--prelight-analytic'], 'turns')
        line(within_bars(s) and c['stale'] == 0 and c['elsewhere'] > 0, 'F1 --turns 8 + analytic on Bryan: no unbaked reads (stale reads %d), %d groups taken with the ops already dropped by the turn, mean %.3f p99.9 %d over-12 %.4f%% (bars hold)' % (c['stale'], c['elsewhere'], s['mean'], s['p999'], 100 * s['gt12']))
        # F2: a reader before the bake (the planted reorder) is twinned with the remembered lights, not declined
        s, c = bars('bryan', list(range(12, 91, 4)), [], ['--prelight-analytic', '--prelight-analytic-fault', 'early_reader'], 'early')
        line(within_bars(s) and c['orphan'] > 0 and 'comes before the bake' not in c['why'], 'F2 planted reorder (the first reader of every group before its bake, from frame 10): twinned with the remembered lights (orphan readers %d), none declined for it (%s), mean %.3f p99.9 %d' % (c['orphan'], c['why'] or 'none', s['mean'], s['p999']))
        # F3: Check B frame 228 has readers and no bake; marked as a split part it must still be twinned
        s, c = bars('brock', [220, 222, 224, 226, 228, 230, 232, 234], ['--part-every', '2'], ['--part-every', '2', '--prelight-analytic'], 'part')
        line(within_bars(s) and c['stale'] == 0 and c['orphan'] > 0, 'F3 Check B frames 220-234 with every 2nd frame a split part: stale reads %d, orphan readers %d, mean %.3f p99.9 %d over-12 %.4f%%' % (c['stale'], c['orphan'], s['mean'], s['p999'], 100 * s['gt12']))
        # F9: the switch on for frames 0-227 and off from 228 (a frame with no bake): the readers keep the remembered lights until the stream bakes again
        s, c = bars('brock', list(range(216, 250, 2)), [], ['--prelight-analytic', '--prelight-analytic-off-from', '228'], 'off')
        line(within_bars(s) and c['stale'] == 0 and c['orphan'] > 0, 'F9 switch on to frame 227, off from 228: stale reads %d, orphan readers %d (twinned after the switch went off), mean %.3f p99.9 %d over-12 %.4f%%' % (c['stale'], c['orphan'], s['mean'], s['p999'], 100 * s['gt12']))
        # F4: a float-format cube is declined
        p = Arm(env, 'bryan', 'plant_float', ['--prelight-analytic', '--prelight-analytic-fault', 'float_format'])
        clean(p, 'bryan float_format')
        ne, _ = arms['bryan']['off'].pics_equal(p)
        line(p.c['taken'] == 0 and p.c['groups'] > 0 and '8-bit UNORM' in p.c['why'] and ne == 0, 'F4 planted float-format cube: every group DECLINED for it (%d of %d: %s), pictures equal the stream (%d differ)' % (p.c['declined'], p.c['groups'], p.c['why'], ne))
        # F6: continuity at the lobes' pivots, from the captured constants
        r = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'l1_pivot.py'), env.caps['bryan']], capture_output=True, text=True, timeout=900)
        r2 = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'l1_pivot.py'), env.caps['brock']], capture_output=True, text=True, timeout=900)
        line(r.returncode == 0 and r2.returncode == 0, 'F6 the response curve is continuous at both lobes\' pivots on every captured frame (no step to soften): %s | %s' % (r.stdout.strip().splitlines()[-1], r2.stdout.strip().splitlines()[-1]))
        # F7: op counts
        r = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'l1_opcount.py')], capture_output=True, text=True, timeout=300)
        print('      ' + '\n      '.join(r.stdout.strip().splitlines()))
        line(r.returncode == 0, 'F7 the precomputed per-pixel function has fewer lane ops than the bake program as written (exit %d)' % r.returncode)
    if 'counts' in only:
        c_off, _ = rplay_counts(env, 'r_off', [])
        c_on, t1 = rplay_counts(env, 'r_on', ['--prelight-analytic'])
        print('      renderer counts a frame (Bryan, frames >= 10, G12 copy batch ON in both arms): off %s' % {k: round(v, 2) for k, v in c_off.items()})
        print('      renderer counts a frame (Bryan, frames >= 10, G12 copy batch ON in both arms): on  %s' % {k: round(v, 2) for k, v in c_on.items()})
        line(c_off['passes'] - c_on['passes'] >= 4.99, 'rplay: passes a frame %.2f -> %.2f (-%.2f: the strip passes are gone)' % (c_off['passes'], c_on['passes'], c_off['passes'] - c_on['passes']))
        line(c_on['barriers'] < c_off['barriers'], 'rplay: barriers a frame fall (%.2f -> %.2f)' % (c_off['barriers'], c_on['barriers']))
        line(c_on['copies'] < c_off['copies'], 'rplay: copies a frame fall (%.2f -> %.2f)' % (c_off['copies'], c_on['copies']))
        line(c_on['pipeline_binds'] <= c_off['pipeline_binds'], 'rplay: pipeline binds a frame do not rise (%.2f -> %.2f)' % (c_off['pipeline_binds'], c_on['pipeline_binds']))
    if 'plants' in only and 'brock' in arms and 'off_pics' in arms['brock']:
        cap = 'brock'   # the lights move on Check B (they are constant over Bryan's entrance), so a stale constant set shows there
        frames = frames_by_cap[cap][::3]
        off_p, drop_p = arms[cap]['off_pics'], arms[cap]['drop_pics']
        off, on = arms[cap]['off'], arms[cap]['on']

        def plant_pics(name, extra):
            per, text = stats_for(env, cap, frames, extra, off_p, drop_p, 'plant_' + name)
            return summarise(per), text
        for name in ('light_missing', 'wrong_space', 'alpha_dropped', 'stale_k'):
            s, _ = plant_pics(name, ['--prelight-analytic', '--prelight-analytic-fault', name])
            over = s['mean'] > PLANT_FACTOR * BAR_MEAN or s['p999'] > BAR_P999 or s['gt12'] > BAR_GT12
            line(over and not within_bars(s), 'twin %s is CAUGHT by the histogram bars: mean %.3f (bar %.1f), p99.9 %d (bar %d), over 12 levels %.4f%% (bar %.2f%%)' % (name, s['mean'], BAR_MEAN, s['p999'], BAR_P999, 100 * s['gt12'], 100 * BAR_GT12))
        p = Arm(env, cap, 'plant_keep_bake', ['--prelight-analytic', '--prelight-analytic-fault', 'keep_bake'])
        line(p.c['taken'] > 0 and abs(p.copies - off.copies) < 0.01 and abs(p.draws - off.draws) < 0.01 and abs(p.targets - off.targets) < 0.01,
             'twin keep_bake is CAUGHT by the counts: copies %.2f draws %.2f targets %.2f a frame, the clean arm has %.2f / %.2f / %.2f (off %.2f / %.2f / %.2f); taken %d' % (
                 p.copies, p.draws, p.targets, on.copies, on.draws, on.targets, off.copies, off.draws, off.targets, p.c['taken']))
        p = Arm(env, cap, 'plant_phantom', ['--prelight-analytic', '--prelight-analytic-fault', 'phantom_reader'])
        line(p.c['declined'] > 0 and 'no analytic twin' in p.c['why'] and p.c['untwinned'] == 0, 'phantom_reader alone: the group with a reader that has no twin is declined (%s)' % p.c['why'])
        p = Arm(env, cap, 'plant_no_reader_check', ['--prelight-analytic', '--prelight-analytic-fault', 'phantom_reader,no_reader_check'])
        line(p.c['untwinned'] > 0, 'twin no_reader_check (a reader without a twin left unchecked) is CAUGHT: %d groups taken although a reader would sample the unbaked cube (the clean arm: %d)' % (p.c['untwinned'], on.c['untwinned']))
        p = Arm(env, cap, 'plant_ignore_overlap', ['--prelight-analytic', '--prelight-analytic-fault', 'ignore_overlap', '--dead-work', '2', '--dead-work-fault', 'cut_live'])
        line(p.c['overlapping'] > 0, 'twin ignore_overlap is CAUGHT: %d groups taken with an op in G4\'s drop set (the clean arm: 0)' % p.c['overlapping'])
        s, text = plant_pics('no_twin', ['--prelight-analytic', '--prelight-analytic-fault', 'no_twin'])
        c, _ = parse_census(text)
        line(c['missed'] > 0 and (s['mean'] > PLANT_FACTOR * BAR_MEAN or s['p999'] > BAR_P999), 'twin no_twin (the readers keep the stream program and the cube nobody baked) is CAUGHT: readers missed %d, mean %.3f, p99.9 %d' % (c['missed'], s['mean'], s['p999']))
    print('%s: %d failure(s)' % ('FAIL' if FAILS else 'ok', FAILS))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
