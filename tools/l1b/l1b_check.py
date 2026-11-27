r"""S102 lane L1b offline proof, 0 game runs, 0 Surface runs: the pre-light cubes written by ONE compute dispatch (src\renderer_adapter\prelight_compute.h, switch wwe2k14_rdr_prelight_compute; shader
src\renderer\shaders\rdr_light_cubes.comp; renderer: renderer.cpp LightCubesInit / RecordLightCubes, light_cubes.h).
  py -I tools\l1b\l1b_check.py [--root DIR] [--only census,wiring,unit,selftest,replay,counts,plants,pics,opcount] [--captures bryan,brock]

fview sweeps (--sweep: the picture, the frame the renderer is given: ops / targets / draws / copies, and with FVIEW_SWEEP_CUBE_DUMP the bytes of every layer of every cube bound at fetch slot 13) on the
the Bryan capture and on Check B (brock, every 4th frame's cubes dumped), under the PLAYER CONFIG (the shipped promotions bundle: --crowd-ours ... --shadows-ours; G12's copy batch ON in both arms):
  main      switch off (the stream's bake) vs on (the dispatch): validation read from the error count AND the exit code; every slot taken (census), none declined, the audit counters 0; the cubes' texels compared
            with the stream's bake (l1b_cubes.py: |off - on| per channel, the bars below); the pictures' differing frames counted; dispatches = frames taken; passes (targets) fall by exactly the taken
            slots, draws by the taken slots, copies by 6 x taken.
  alias2    --copy-alias 2 on: B equals the in-place B (the group's copies are an op another hook drops).
  nocompute --no-compute: every group declined for it (counted), nothing dropped, equal to the stream.
  dead      --dead-work 2 --dead-work-fault cut_live: the cut slots are declined (G4's), no op both cut and dispatched.
  layered   --prelight-layered too: this lever takes the groups first and C1 declines them ("another planner's drop set"); equal to the compute-only arm.
unit      (review F3 / F4) the planner on synthetic frames (prelight_compute_test.cpp): five slots taken, a ScreenScale that leaves the mesh declined, 17 slots take nothing past the tag word's 8, a cube read between
            the dispatch and its slot declined; each of the three planted faults makes it FAIL.
selftest  (review F1 / F5) rplay --light-cube-selftest: a synthetic frame with a dispatch into a lasting cube nothing reads: Full records it, DisplayOnly does not, PersistentOnly does; a dispatch naming kNoImage is
            dropped whole with a counted Problem; `--light-cube-plant 4` (the first version's rule) must FAIL it.
pics      the whole-picture difference (l1b_pics.py) on Bryan frames 20, 60, 85.
rplay renderer counts (Bryan, frames >= 10, copy batch ON in both arms): passes, copies, barriers, pipeline binds, dispatches a frame; nothing rises but dispatches (+1 a frame).
Planted twins (each must be CAUGHT): shifted_screen (declined), --light-cube-plant 3 (the init fails: every group declined, equal to the stream), half_texel_dropped, stale_k (the cube bars), --light-cube-plant 2 (face axis: cubes differ), --light-cube-plant 1 (the barrier left out: validation errors, exit 4),
keep_ops (the counts do not fall), phantom_reader + no_reader_check (taken_read_between > 0), phantom_reader alone (declined, "read between"), ignore_overlap + the dead cut (taken_overlapping > 0), bad_mesh (every
group declined for the mesh). Also: the mesh and clip transform from the captures alone (l1b_mesh.py), the wiring with each link removed in a text copy, the lints, the op count (l1b_opcount.py).
Exit 1 on any FAIL; a missing capture or exe FAILS."""
import argparse
import csv
import json
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
FAILS = 0
# The player config the captures can replay (the shipped default bundle's capture-capable part: shadows ours, casters, plane, the owned small passes; the other promotions need scene records the captures do not carry)
BUNDLE = ['--shadows-ours', '--shadow-casters-ours', '--plane-shadow-ours', '--small-passes-ours']
BUNDLE_FULL = ['--crowd-ours', '--crowd-cards', '--arena-ours', '--wrestlers-ours', '--caw-ours', '--ring-ours', '--ui-ours', '--ui-quads', '--ui-text-ours', '--flash-ours', '--props-ours', '--post-ours',
          '--movie-ours', '--screens-ours', '--shadow-casters-ours', '--rim-ours', '--circles-ours', '--plane-shadow-ours', '--small-passes-ours', '--rim-chain-ours', '--particles-ours', '--shadows-ours']
# The bars (measured on Bryan + Check B, docs\renderer\PRELIGHT_DIRECT.md section 9.3; the clean arm sits under each, every planted twin goes over at least one)
BAR_MAX = 1            # the largest |off - on| of any channel of any texel of any cube, levels of 255
BAR_OVER1 = 0.0        # texels with a channel off by more than 1 level: none
BAR_D1 = 0.02          # the share of channel values that differ by exactly 1 level (the bake rounds, the shader's storage store rounds: they can split a .5)
PLANT_FACTOR = 3.0


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
        self.work = os.path.join(root, 'out', 'l1b')
        os.makedirs(self.work, exist_ok=True)
        self.caps = {'bryan': os.path.join(self.main, 'out', 'lane86', 'bryan', 'capture'), 'brock': os.path.join(self.main, 'out', 'lane86', 'brock', 'capture')}
        self.every = {'bryan': 1, 'brock': 4}   # the cubes are dumped every N-th frame (Check B has 341 frames)


CENSUS = re.compile(r'\[rdr\]\[prelight_compute\] frames (\d+) \(with slots (\d+), taken (\d+)\), groups (\d+), taken (\d+), declined (\d+)(?: \((.*?)\))? \| dispatches (\d+), cubes written (\d+), ops dropped (\d+) \| '
                    r'audit: taken read between (\d+), taken overlapping (\d+) \| planner ([\d.]+) us a frame, meshes checked (\d+)')
KEYS = ('frames', 'with_slots', 'frames_taken', 'groups', 'taken', 'declined', 'why', 'dispatches', 'cubes', 'dropped', 'read_between', 'overlapping', 'plan_us', 'meshes')


def parse_census(text):
    m = CENSUS.search(text)
    vals = {}
    if m:
        for k, x in zip(KEYS, m.groups()):
            vals[k] = (x or '') if k == 'why' else float(x) if k == 'plan_us' else int(x or 0)
    else:
        vals = dict.fromkeys(KEYS, 0)
        vals['why'] = ''
    return vals, bool(m)


class Arm:
    def __init__(self, env, cap, name, extra, base=None, dump=False, exe=None, env_extra=None):
        out = os.path.join(env.work, '%s_%s.csv' % (cap, name))
        if os.path.exists(out):
            os.remove(out)
        self.dump_dir = None
        e = dict(os.environ, FVIEW_SWEEP_COUNTS='1', FVIEW_SWEEP_LAYERS='1')
        if dump:
            self.dump_dir = os.path.join(env.work, '%s_%s_cubes' % (cap, name))
            shutil.rmtree(self.dump_dir, ignore_errors=True)
            os.makedirs(self.dump_dir)
            e['FVIEW_SWEEP_CUBE_DUMP'] = self.dump_dir
            e['FVIEW_CUBE_DUMP_EVERY'] = str(env.every[cap])
        e.update(env_extra or {})
        base = list(BUNDLE) + ['--copy-batch', 'on'] if base is None else list(base)
        cmd = [exe or env.fview, '--capture', env.caps[cap], '--validation', '1', '--sweep', out, '--sweep-slot', '13'] + base + list(extra)
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=3000, env=e, errors='replace')
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
        self.ops = sum(int(r['gpu_ops']) for r in self.rows) / n
        self.c, self.census_line = parse_census(self.text)

    def valid(self):
        return self.rc == 0 and self.errors == 0 and bool(self.rows)

    def pictures_differ(self, other):
        n = min(len(self.rows), len(other.rows))
        return sum(1 for i in range(n) if self.rows[i]['picture'] != other.rows[i]['picture']), len(self.rows) != len(other.rows)

    def cubes_differ(self, other):
        n = min(len(self.rows), len(other.rows))
        return sum(1 for i in range(n) if self.rows[i]['slot13_hash'] != other.rows[i]['slot13_hash'])


def clean(a, text):
    line(a.valid(), '%s: validation clean (error count %d, exit code %d, %d frames)' % (text, a.errors, a.rc, len(a.rows)))


def cube_stats(off, on):
    """the cube dumps of two arms compared texel by texel -> dict (l1b_cubes.compare)"""
    import l1b_cubes
    return l1b_cubes.compare(off.dump_dir, on.dump_dir)


def cube_bars(res, text):
    t = max(1, res['values'])
    ok = res['missing'] == 0 and res['max'] <= BAR_MAX and res['texels_over_1'] <= BAR_OVER1 * res['texels'] and res['d1'] / t <= BAR_D1
    line(ok, '%s: cube texels vs the stream bake: |off - on| 0 -> %.4f %%, 1 -> %.4f %%, 2 -> %.4f %%, 3+ -> %.4f %%, max %d (bars: max <= %d, none over 1, <= %.0f %% at 1); %d texels over 1 of %d; worst: %s' % (
        text, 100.0 * res['d0'] / t, 100.0 * res['d1'] / t, 100.0 * res['d2'] / t, 100.0 * res['d3p'] / t, res['max'], BAR_MAX, 100.0 * BAR_D1, res['texels_over_1'], res['texels'], res['worst']))
    return ok


def over_bars(res):
    t = max(1, res['values'])
    return res['max'] > BAR_MAX or res['texels_over_1'] > 0 or res['d1'] / t > BAR_D1 * PLANT_FACTOR


WIRING = [
    ('switch table row (PerFrame, default 0)', 'src/renderer_adapter/rdr_switches.h', 'X(PrelightCompute, "wwe2k14_rdr_prelight_compute", PerFrame, 0)'),
    ('cvar defined', 'src/renderer_adapter/reader.h', 'REXCVAR_DEFINE_BOOL(wwe2k14_rdr_prelight_compute, false'),
    ('the switch is read with the guest frame (BoolSeen)', 'src/renderer_adapter/binder.h', 'rdr_sw::BoolSeen(rdr_sw::kPrelightCompute, prelight_compute::g_fview.load(std::memory_order_relaxed), frame.header.guest_frame)'),
    ('the cubes get storage usage only when the device can write them', 'src/renderer_adapter/binder.h', 'renderer_.compute_supported() && renderer_.StorageTargetable(desc.format)'),
    ('the planner runs after L1 and before C1', 'src/renderer_adapter/binder.h', 'PreLightComputeBegin(frame, capture, stats);'),
    ('C1 sees this drop set (declines on overlap)', 'src/renderer_adapter/binder.h', 'ctx.others.push_back(pc_plan_.drop.data());'),
    ('the group copies are an op another hook drops (copy alias)', 'src/renderer_adapter/binder.h', 'if (pc_on_ && pc_plan_.drop[p]) return true;'),
    ('the dispatch is issued where the first slot began', 'src/renderer_adapter/binder.h', 'if (pc_on_ && op_index == pc_plan_.first_op) PreLightComputeDispatch(frame, builder);'),
    ('a live census line every 600 frames', 'src/renderer_adapter/binder.h', 'rdr::Log("%s", pc_planner_.Report().c_str());'),
    ('F1: the dispatch is persistent by the copies\' rule (lasting / carries state / never read)', 'src/renderer/renderer.cpp', '(!old_rule && ((c.desc.usage & kUsageLasting) || !c.ever_read))'),
    ('F2: the pipelines are made at startup, compute_supported() is true only once they exist', 'src/renderer/renderer.cpp', 'impl_->lc_state == 1 && !g_force_no_compute'),
    ('F2: the renderer reads the start-time switch', 'src/renderer/renderer.cpp', 'if (options.light_cube_compute && renderer->impl_->device.caps().compute) renderer->impl_->LightCubesInit();'),
    ('F3: both axes of the mapping are held to the game\'s', 'src/renderer_adapter/prelight_compute.h', 'if (!axis_ok(xs, xo) || !axis_ok(ys, yo)) return kClipTransform;'),
    ('F4: groups past the tag word are declined', 'src/renderer_adapter/prelight_compute.h', 'i >= kTagGroups && !Planted(Fault::NoIndexCap) ? kTooMany'),
    ('F5: a dispatch naming a missing image is dropped whole', 'src/renderer/renderer.cpp', 'Problem("light_cube_bad", "op %u: a dispatch of %u cubes names an image that is not there"'),
    ('F7: the direction table is made again only when K[15] changes', 'src/renderer/renderer.cpp', 'std::memcmp(k15, lc_table_k, sizeof(k15)) != 0'),
    ('the renderer: a dispatch is a step', 'src/renderer/renderer.cpp', 'steps.push_back({StepKind::Dispatch, op.index, false});'),
    ('the renderer: the cubes go to GENERAL in ONE barrier flush', 'src/renderer/renderer.cpp', 'Transition(*cube[k], 0, 6, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, true, true);'),
    ('the renderer: one vkCmdDispatch for the cubes', 'src/renderer/renderer.cpp', 'vkCmdDispatch(cmd, kLightCubeSize / kLightCubeGroups, kLightCubeSize / kLightCubeGroups, n * 6);'),
    ('the renderer: a cube a dispatch writes is checked for layout when sampled (copy_dest)', 'src/renderer/renderer.cpp', 'images[d.cube[k]].copy_dest = true;'),
        ('the renderer: the barrier plant', 'src/renderer/renderer.cpp', 'if (plant == 1)   // planted: the barrier between the dispatch and the readers is left out'),
    ('the device: storage usage is asked only for a format the device can write', 'src/renderer/renderer.cpp', 'format == Format::RGBA8 && impl_->device.FormatHas(ToVk(format, impl_->device.caps()), VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT'),
]


def wiring(root):
    for what, rel, needle in WIRING:
        path = os.path.join(root, rel)
        try:
            sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'split'))
            from read_unit import read_path  # a split file is read with its parts expanded (tools/split/read_unit.py)
            text = read_path(path)
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
                                ('device_features_lint (the device creation is as it was: no feature, no extension)', os.path.join('tools', 'renderer', 'device_features_lint.py'), ['--root', root]),
                                ('spv_lint (every SPIR-V header but this lane\'s three compute shader headers byte-equal to the tip\'s)', os.path.join('tools', 'l1b', 'spv_lint.py'), ['--root', root])):
        if not os.path.exists(os.path.join(root, script)):
            line(False, '%s: the script %s is missing' % (what, script))
            continue
        r = subprocess.run([sys.executable, '-I', os.path.join(root, script)] + extra, capture_output=True, text=True, timeout=300)
        line(r.returncode == 0, '%s passes (exit %d)' % (what, r.returncode))


def rplay_counts(env, name, extra):
    out = os.path.join(env.work, name + '.json')
    p = subprocess.run([env.rplay, '--capture', env.caps['bryan'], '--frames', '91', '--counts', out, '--copy-batch', '1', '--shadows-ours', '--plane-shadow-ours', '--small-passes-ours'] + extra, capture_output=True, text=True, timeout=1500, errors='replace')
    if p.returncode != 0:
        raise RuntimeError('rplay %s exit %d: %s' % (name, p.returncode, (p.stdout + p.stderr)[-600:]))
    tot = {'copies': 0, 'barriers': 0, 'pipeline_binds': 0, 'passes': 0, 'dispatches': 0, 'draws': 0}
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
                    tot['dispatches'] += d['total'].get('dispatches', 0)
                    tot['draws'] += d['total'].get('draws', 0) + d['total'].get('indexed_draws', 0)
                    tot['passes'] += d.get('passes', 0)
                    n += 1
    return {k: v / max(1, n) for k, v in tot.items()}, p.stdout + p.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--only', default='census,wiring,unit,selftest,replay,counts,plants,pics,opcount')
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
        for c in a.captures.split(','):
            r = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'l1b_mesh.py'), env.caps[c]], capture_output=True, text=True, timeout=1800)
            print(r.stdout.strip())
            line(r.returncode == 0, '%s: the pre-light mesh equals the shader\'s model on every sampled frame, the texel -> grid mapping is the expected one (exit %d)' % (c, r.returncode))
    if 'wiring' in only:
        wiring(env.root)
    if 'unit' in only:   # review F3 / F4: the planner on synthetic frames, three planted faults
        exe = os.path.join(env.work, 'prelight_compute_test.exe')
        clang = r'C:\Program Files\LLVM\bin\clang++.exe'
        c = subprocess.run([clang, '-std=c++20', '-O1', '-D_CRT_SECURE_NO_WARNINGS', '-I', os.path.join(env.root, 'src'), '-I', os.path.join(env.root, 'src', 'renderer_adapter'),
                            os.path.join(env.root, 'src', 'renderer_adapter', 'prelight_compute_test.cpp'), '-o', exe], capture_output=True, text=True, timeout=600)
        if c.returncode != 0:
            line(False, 'prelight_compute_test.cpp did not compile: %s' % c.stderr[-400:])
        else:
            r = subprocess.run([exe], capture_output=True, text=True, timeout=120)
            line(r.returncode == 0 and 'PASS: 0 failures' in r.stdout, 'planner unit test: five slots taken, a mapping outside the mesh declined, 17 slots take nothing past 8, a read between declined (%s)' % (r.stdout.strip().splitlines() or ['no output'])[-1])
            for plant in ('plant_no_index_cap', 'plant_shifted_screen', 'plant_no_reader_check'):
                r = subprocess.run([exe, plant], capture_output=True, text=True, timeout=120)
                line(r.returncode != 0, 'planner unit test, planted %s: the test FAILS it (exit %d)' % (plant, r.returncode))
    if 'selftest' in only:   # review F1 / F5
        r = subprocess.run([env.rplay, '--light-cube-selftest', '--validation', '1'], capture_output=True, text=True, timeout=600, errors='replace')
        out = (r.stdout or '') + (r.stderr or '')
        print('\n'.join(l for l in out.splitlines() if l.startswith(('PASS', 'FAIL', '[rplay][lc_selftest]'))))
        line(r.returncode == 0 and '[rplay][lc_selftest] PASS' in out, 'F1 / F5 selftest: Full 1, DisplayOnly 0, PersistentOnly 1 dispatch for a lasting cube; a dispatch naming kNoImage dropped whole with a counted Problem (exit %d)' % r.returncode)
        r = subprocess.run([env.rplay, '--light-cube-selftest', '--light-cube-plant', '4'], capture_output=True, text=True, timeout=600, errors='replace')
        line(r.returncode != 0 and '[rplay][lc_selftest] FAIL' in ((r.stdout or '') + (r.stderr or '')), 'F1 plant (--light-cube-plant 4, the first version\'s persistence rule): the selftest FAILS it (exit %d)' % r.returncode)
    if 'pics' in only:
        for cap, frames in (('bryan', '20,60,85'), ('brock', '100,150,200')):
            if cap in a.captures.split(','):
                r = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'l1b_pics.py'), '--root', env.root, '--cap', cap, '--frames', frames], capture_output=True, text=True, timeout=3000)
                print(r.stdout.strip())
                line(r.returncode == 0, '%s: whole-picture difference of the dispatch against the stream\'s bake: at most 2 levels on every pixel (exit %d)' % (cap, r.returncode))
    arms = {}
    if 'replay' in only or 'plants' in only:
        for cap in a.captures.split(','):
            off = Arm(env, cap, 'off', [], dump=True)
            on = Arm(env, cap, 'on', ['--prelight-compute'], dump=True)
            arms[cap] = (off, on)
            if 'replay' not in only:
                continue
            clean(off, '%s off' % cap)
            clean(on, '%s on' % cap)
            c = on.c
            line(on.census_line and c['groups'] > 0 and c['taken'] == c['groups'] and c['declined'] == 0 and c['read_between'] == 0 and c['overlapping'] == 0,
                 '%s: census groups %d, taken %d, declined %d (%s), audit: read between %d, overlapping %d (frames with slots %d, taken %d; mesh checks %d; planner %.1f us a frame)' % (
                     cap, c['groups'], c['taken'], c['declined'], c['why'], c['read_between'], c['overlapping'], c['with_slots'], c['frames_taken'], c['meshes'], c['plan_us']))
            line(c['dispatches'] == c['frames_taken'] and c['cubes'] == c['taken'] and c['dropped'] == 8 * c['taken'],
                 '%s: dispatches %d = frames taken %d; cubes written %d = taken slots %d; ops dropped %d = 8 x taken' % (cap, c['dispatches'], c['frames_taken'], c['cubes'], c['taken'], c['dropped']))
            builds = re.findall(r'direction table was made (\d+) time', on.text)
            line(builds and int(builds[-1]) == 1, '%s: the direction table was made %s time(s) in the whole run (F7: once, then only when K[15] changes)' % (cap, builds[-1] if builds else '?'))
            n = max(1, len(on.rows))
            taken_a_frame = c['taken'] / n
            line(abs((off.targets - on.targets) - taken_a_frame) < 0.01 and abs((off.draws - on.draws) - taken_a_frame) < 0.01 and abs((off.copies - on.copies) - 6 * taken_a_frame) < 0.01,
                 '%s: a frame: passes (targets) %.2f -> %.2f, draws %.2f -> %.2f, copies %.2f -> %.2f: each falls by the census (%.2f, %.2f, %.2f)' % (
                     cap, off.targets, on.targets, off.draws, on.draws, off.copies, on.copies, taken_a_frame, taken_a_frame, 6 * taken_a_frame))
            res = cube_stats(off, on)
            cube_bars(res, cap)
            pics, mism = off.pictures_differ(on)
            line(not mism, '%s: %d of %d frames have a different picture hash (information: the cubes differ by rounding only; see the cube bars)' % (cap, pics, len(off.rows)))
            # the picture difference, in levels (l1_pics' wrestler-pixel histogram): reported, bars below
            # --copy-alias 2
            a_off = Arm(env, cap, 'alias2_off', ['--copy-alias', '2'])
            a_on = Arm(env, cap, 'alias2_on', ['--copy-alias', '2', '--prelight-compute'])
            clean(a_on, '%s copy-alias 2 on' % cap)
            p_ao, _ = a_off.pictures_differ(a_on)
            p_on, _ = on.pictures_differ(a_on)
            line(a_on.c['taken'] == c['taken'] and p_on == 0, '%s: --copy-alias 2 on equals the in-place run (pictures %d frames differ), the same slots taken (%d vs %d)' % (cap, p_on, a_on.c['taken'], c['taken']))
    if 'replay' in only and 'bryan' in a.captures.split(','):
        off, on = arms['bryan']
        nc = Arm(env, 'bryan', 'nocompute', ['--prelight-compute', '--no-compute'])
        clean(nc, 'bryan --no-compute')
        pics, _ = off.pictures_differ(nc)
        line(nc.c['taken'] == 0 and nc.c['groups'] > 0 and 'cannot dispatch' in nc.c['why'] and nc.c['dropped'] == 0 and pics == 0 and abs(nc.copies - off.copies) < 1e-9 and abs(nc.targets - off.targets) < 1e-9,
             'bryan --no-compute: every group declined for it (%d of %d: %s), nothing dropped, equal to the stream path (pictures %d frames differ), copies and passes unchanged' % (nc.c['declined'], nc.c['groups'], nc.c['why'], pics))
        dead_base = ['--dead-work', '2', '--dead-work-fault', 'cut_live']
        d_off = Arm(env, 'bryan', 'dead_off', dead_base)
        d_on = Arm(env, 'bryan', 'dead_on', dead_base + ['--prelight-compute'])
        clean(d_on, 'bryan dead cut')
        line("dead skip" in d_on.c['why'] and d_on.c['taken'] > 0 and d_on.c['overlapping'] == 0, 'bryan dead cut: G4 cut slots declined (%s), the others taken (%d), no op both cut and dispatched (overlapping %d)' % (d_on.c['why'], d_on.c['taken'], d_on.c['overlapping']))
        l_on = Arm(env, 'bryan', 'layered_on', ['--prelight-compute', '--prelight-layered'])
        clean(l_on, 'bryan compute + layered')
        pl, _ = on.pictures_differ(l_on)
        line(l_on.c['taken'] == on.c['taken'] and pl == 0, 'bryan compute + layered: this lever took the same slots (%d vs %d), C1 declined them (pictures equal to compute-only: %d frames differ)' % (l_on.c['taken'], on.c['taken'], pl))
    if 'counts' in only:
        try:
            base, _ = rplay_counts(env, 'counts_off', [])
            new, text = rplay_counts(env, 'counts_on', ['--prelight-compute'])
            print('rplay counts a frame (Bryan, frames >= 10): off %s' % {k: round(v, 2) for k, v in base.items()})
            print('                                            on  %s' % {k: round(v, 2) for k, v in new.items()})
            line(new['passes'] <= base['passes'] - 4.9 and new['copies'] <= base['copies'] - 4.9, 'rplay counts: passes %.2f -> %.2f, copies %.2f -> %.2f (against G12\'s copy batch: the 5 batched copy commands go)' % (base['passes'], new['passes'], base['copies'], new['copies']))
            line(new['barriers'] < base['barriers'] and new['pipeline_binds'] <= base['pipeline_binds'] + 1.0001, 'rplay counts: barriers %.2f -> %.2f (down), pipeline binds %.2f -> %.2f (the dispatch\'s one bind at most)' % (base['barriers'], new['barriers'], base['pipeline_binds'], new['pipeline_binds']))
            line(abs(new['dispatches'] - 1.0) < 0.01 and base['dispatches'] == 0, 'rplay counts: dispatches %.2f -> %.2f (one a frame)' % (base['dispatches'], new['dispatches']))
        except Exception as error:  # noqa: BLE001
            line(False, 'rplay counts: %s' % error)
    if 'plants' in only and 'bryan' in arms:
        off, on = arms['bryan']
        base_on = ['--prelight-compute']
        # planted twins: each must be caught
        for fault, why in (('half_texel_dropped', 'the cube bars'), ('stale_k', 'the cube bars')):
            p = Arm(env, 'bryan', 'plant_' + fault, base_on + ['--prelight-compute-fault', fault], dump=True)
            res = cube_stats(off, p)
            t = max(1, res['values'])
            line(p.valid() and over_bars(res), 'plant %s CAUGHT (%s): max %d, %d texels over 1, 1-level share %.2f %% (bars: max <= %d, none over 1, <= %.0f %%)' % (fault, why, res['max'], res['texels_over_1'], 100.0 * res['d1'] / t, BAR_MAX, 100.0 * BAR_D1 * PLANT_FACTOR))
        p = Arm(env, 'bryan', 'plant_axis', base_on + ['--light-cube-plant', '2'], dump=True)
        res = cube_stats(off, p)
        line(over_bars(res), 'plant face axis (--light-cube-plant 2, the swapped-face shader) CAUGHT: max %d, %d texels over 1' % (res['max'], res['texels_over_1']))
        p = Arm(env, 'bryan', 'plant_barrier', base_on + ['--light-cube-plant', '1'])
        line(p.errors > 0 and p.rc == 4, 'plant barrier left out (--light-cube-plant 1) CAUGHT by validation: %d errors, exit code %d (a clean run reads 0 and 0)' % (p.errors, p.rc))
        p = Arm(env, 'bryan', 'plant_keep_ops', base_on + ['--prelight-compute-fault', 'keep_ops'])
        line(p.valid() and abs(p.copies - off.copies) < 1e-9 and abs(p.targets - off.targets) < 1e-9,
             'plant keep_ops CAUGHT by the counts: copies %.2f (the stream %.2f) and passes %.2f (%.2f) do not fall, the clean arm\'s do (%.2f / %.2f)' % (p.copies, off.copies, p.targets, off.targets, on.copies, on.targets))
        p = Arm(env, 'bryan', 'plant_phantom', base_on + ['--prelight-compute-fault', 'phantom_reader'])
        line(p.valid() and p.c['read_between'] == 0 and 'read between' in p.c['why'] and p.c['taken'] < p.c['groups'], 'plant phantom_reader: the group is DECLINED (%s), audit taken-read-between %d' % (p.c['why'], p.c['read_between']))
        p = Arm(env, 'bryan', 'plant_phantom_nocheck', base_on + ['--prelight-compute-fault', 'phantom_reader,no_reader_check'])
        line(p.c['read_between'] > 0, 'plant phantom_reader + no_reader_check CAUGHT by the audit: taken with a read between %d (a clean arm reads 0)' % p.c['read_between'])
        p = Arm(env, 'bryan', 'plant_overlap', base_on + ['--dead-work', '2', '--dead-work-fault', 'cut_live', '--prelight-compute-fault', 'ignore_overlap'])
        line(p.c['overlapping'] > 0, 'plant ignore_overlap + the dead cut CAUGHT by the audit: taken overlapping %d (a clean arm reads 0)' % p.c['overlapping'])
        p = Arm(env, 'bryan', 'plant_shifted', base_on + ['--prelight-compute-fault', 'shifted_screen'])
        line(p.valid() and p.c['taken'] == 0 and 'clip transform' in p.c['why'], 'plant shifted_screen CAUGHT (review F3): every group declined as the clip transform (%d of %d: %s)' % (p.c['declined'], p.c['groups'], p.c['why']))
        p = Arm(env, 'bryan', 'plant_init', base_on + ['--light-cube-plant', '3'])
        pics, _ = off.pictures_differ(p)
        line(p.valid() and p.c['taken'] == 0 and 'cannot dispatch' in p.c['why'] and pics == 0 and abs(p.copies - off.copies) < 1e-9, 'plant 3 (the compute init fails, review F2): every group declined (%s), nothing dropped, pictures equal the stream (%d frames differ)' % (p.c['why'], pics))
        p = Arm(env, 'bryan', 'plant_mesh', base_on + ['--prelight-compute-fault', 'bad_mesh'])
        line(p.valid() and p.c['taken'] == 0 and 'mesh' in p.c['why'], 'plant bad_mesh CAUGHT: every group declined for the mesh (%d of %d: %s)' % (p.c['declined'], p.c['groups'], p.c['why']))
    if 'opcount' in only:
        r = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'l1b_opcount.py')], capture_output=True, text=True, timeout=300)
        print(r.stdout.strip())
        line(r.returncode == 0 and 'RESULT lane_ops=' in r.stdout and 'private_arrays=0' in r.stdout, 'op count of the compute shaders, memory lanes counted, no array in Function / Private memory in the per-frame shader (F6; exit %d)' % r.returncode)
    print('%s: %d failures' % ('FAIL' if FAILS else 'PASS', FAILS))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
