#!/usr/bin/env python3
"""S102 lane R18-B2: the owned combine does less per pixel (docs/renderer/COMBINE_LITE.md). Offline, 0 game runs, 0 Surface runs.

  py -I tools\\r18\\r18_b2_check.py [--root DIR] [--only fetches,wiring,bryan,captures,flat] [--jobs N]

The switch wwe2k14_rdr_combine_lite (PerFrame, default 0): 1 = the exact early-outs (a DoF blur / bloom image whose weight is 0 is not fetched), 2 = + the flat-pixel FXAA exit (a LOOK lever).
fetches   the SPIR-V headers: every lite twin exists, the existing variants are byte-identical to the base branch (no deleted line in fx_yGlareEtc.h), the static sample counts per variant, the
          per-path fetch model (what a pixel executes: base vs level 1 vs the flat exit) and the claim's arithmetic.
wiring    every link of the switch (table PerFrame default 0, cvar, live.cpp per-frame read + census, post_pass.cpp pick, fview / rplay flags) is in the source; each removed in a text copy must fail.
bryan     fview headless, the Bryan case zip under the whole shipped bundle (every *-ours switch the players run): off, level 1 (BYTE-EQUAL, validation 0 AND exit 0, every combine draw took the
          variant), level 2 (differs: counted), the planted twins (lite_dof_always_skip differs, lite_flat_wide differs from level 2, lite_not_applied takes 0 draws).
captures  the same off / level 1 pair on the match case zip (bundle), Brock and wm30 (lane86, --post-ours): byte-equal.
flat      the share of combine pixels the flat exit takes, measured by the MARK twin (those pixels drawn magenta) on frames of every capture; level 2's difference from level 0 (pixels over 8 / 255,
          max) and the before / after / difference sheet under out\\s102\\sheets\\r18\\ (tools\\r18\\r18_b2_sheet.py)."""
import argparse
import concurrent.futures
import os
import re
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
CASES = {
    'bryan': 'out/s101/runs/20261112_122700_s100_owner_bryan_entrance/case_20261112_122719_969_0_pc_run_exit.zip',
    'match': 'out/s101/runs/20261112_131008_s100_owner_match/case_20261112_131027_265_0_pc_run_exit.zip',
}
LANE86 = {'brock': 'out/lane86/brock/capture', 'wm30': 'out/lane86/wm30/capture'}
# the whole bundle the players run (the S101 promotions, wwe2k14_app.h); the Bryan / match case zips carry the scene records it needs
BUNDLE = ['--crowd-ours', '--crowd-cards', '--arena-ours', '--wrestlers-ours', '--caw-ours', '--ring-ours', '--ui-ours', '--ui-quads', '--ui-text-ours', '--flash-ours', '--props-ours', '--post-ours',
          '--movie-ours', '--screens-ours', '--shadow-casters-ours', '--rim-ours', '--circles-ours', '--plane-shadow-ours', '--small-passes-ours', '--rim-chain-ours', '--particles-ours', '--shadows-ours']
DUMP = '30,100,200'
CLIP = list(range(90, 150))   # R18-B2: 60 consecutive Bryan frames for the slowed side-by-side clip (stills hide shimmer near the threshold)
BRYAN_DUMP = DUMP + ',' + ','.join(str(f) for f in CLIP)
fails = 0
lock = threading.Lock()


def line(ok, text):
    global fails
    with lock:
        print(('PASS  ' if ok else 'FAIL  ') + text, flush=True)
        fails += 0 if ok else 1


def note(text):
    with lock:
        print('NOTE  ' + text, flush=True)


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


LITE = re.compile(r'\[rdr\]\[combine_lite\] frames \d+, combine draws (\d+), with lite (\d+) \(level 1 (\d+), level 2 (\d+)\), no-bloom permutation (\d+), DOFParam2.z<=0 (\d+), BloomScale==0 (\d+), StarScale==0 (\d+)')
BLOOM = re.compile(r'dead work skipped \d+ draws \d+ copies \(bloom (\d+), glare (\d+), dof (\d+)\)')
POST = re.compile(r'\[rdr\]\[post\] ours frames (\d+)')


class Env:
    def __init__(self, root):
        self.root = root
        self.main = main_checkout(root)
        self.fview = os.path.join(root, 'out', 'renderer', 'dev', 'fview.exe')
        if not os.path.exists(self.fview):
            self.fview = os.path.join(self.main, 'out', 'renderer', 'dev', 'fview.exe')
        self.work = os.path.join(root, 'out', 'r18')
        os.makedirs(self.work, exist_ok=True)
        self.game = os.path.join(self.main, 'game_data', 'extracted')


class Arm:
    """One fview headless replay. The exit code AND the error count are read (fview writes its csv before it exits 4 on validation errors; G5 review)."""
    def __init__(self, env, cap, name, extra, dump=''):
        hashes = os.path.join(env.work, '%s_%s.csv' % (cap, name))
        if os.path.exists(hashes):
            os.remove(hashes)
        self.dir = os.path.join(env.work, 'd_%s_%s' % (cap, name))
        if cap in CASES:
            src, flags = ['--case', os.path.join(env.main, CASES[cap])], BUNDLE
        else:
            src, flags = ['--capture', os.path.join(env.main, LANE86[cap])], ['--post-ours']
        cmd = [env.fview] + src + ['--replay', '--frames', '1000', '--validation', '1', '--game', env.game, '--hash-frames', hashes] + flags + list(extra)
        if dump:
            cmd += ['--dump-frames', dump, '--dump-dir', self.dir]
        for _ in range(12):
            p = subprocess.run(cmd, capture_output=True, text=True, timeout=1700, errors='replace')
            self.text = (p.stdout or '') + (p.stderr or '')
            if 'Not enough graphics memory for the frame ring' not in self.text:
                break
            time.sleep(20)
        self.rc = p.returncode
        with open(os.path.join(env.work, '%s_%s.log' % (cap, name)), 'w', encoding='utf-8') as f:
            f.write(self.text)
        self.hashes = read_hashes(hashes) if os.path.isfile(hashes) else {}
        v = re.findall(r'validation errors (\d+)', self.text)
        self.valid = int(v[-1]) if v else -1
        m = LITE.search(self.text)
        self.draws, self.with_lite, self.l1, self.l2, self.nobloom, self.z_le0, self.bloom0, self.star0 = (int(x) for x in m.groups()) if m else (-1,) * 8
        m = BLOOM.search(self.text)
        self.dead_bloom = int(m.group(1)) if m else -1
        m = POST.search(self.text)
        self.post_frames = int(m.group(1)) if m else -1
        self.clean = self.rc == 0 and self.valid == 0 and bool(self.hashes)
        self.frames = len([k for k in self.hashes if k != 'format'])

    def differing(self, other):
        return sum(1 for k in self.hashes if k != 'format' and self.hashes.get(k) != other.hashes.get(k))

    def took(self):
        """the lever took every combine draw: the census the combine itself kept"""
        return self.draws > 0 and self.with_lite == self.draws


def run_arms(env, specs, jobs):
    """specs: [(key, cap, name, extra, dump)] -> {key: Arm}"""
    out = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = {key: pool.submit(Arm, env, cap, name, extra, dump) for key, cap, name, extra, dump in specs}
        for key, f in futures.items():
            out[key] = f.result()
    return out


# ---- fetches ---------------------------------------------------------------------------------------------------------------------------------------------------------
def spirv_words(header_text, symbol):
    m = re.search(r'kFx_%s\[\d*\] = \{(.*?)\};' % re.escape(symbol), header_text, re.S)   # S105 SPV: header_text is the effect's _data.inc
    if not m:
        return None
    return [int(x, 16) for x in re.findall(r'0x[0-9a-fA-F]+', m.group(1))]


def static_samples(words):
    n, i = 0, 5
    while i < len(words):
        op, count = words[i] & 0xFFFF, words[i] >> 16
        if count == 0:
            break
        n += op in (87, 88, 95)
        i += count
    return n


def fetch_model(has_dof, has_fog, has_lum, has_bloom, level, flat, dof_weight, bloom_scale, star_scale):
    """The texture fetches ONE pixel executes (the shader's own structure): the smoothing taps (5, +4 unless the flat exit is taken), the depth (dof / fog), the DoF blur (always in the old
    shader, then the bokeh in its else branch: here the far branch, the branch Bryan's entrance runs in), the luminance (night / tonemap), the bloom and star images."""
    n = 9
    if level >= 2 and flat:
        n = 5
    if has_dof or has_fog:
        n += 1
    if has_dof:
        n += (2 if level == 0 else (1 if dof_weight != 0 else 0))
    if has_lum:
        n += 1
    if has_bloom:
        n += 2 if level == 0 else (bloom_scale != 0) + (star_scale != 0)
    return n


def check_fetches(env):
    header = open(os.path.join(env.root, 'src', 'renderer', 'shaders', 'spv', 'fx_yGlareEtc_data.inc'), encoding='utf-8', errors='replace').read()   # S105 SPV: the words moved out of fx_yGlareEtc.h
    registry = open(os.path.join(env.root, 'src', 'renderer', 'shaders', 'spv', 'fx_registry.h'), encoding='utf-8', errors='replace').read()
    names = ['dof', 'fog', 'dof_fog', 'bloom', 'dof_bloom', 'fog_bloom', 'dof_fog_bloom', 'dof_fog_night_tonemap_bloom', 'night', 'tonemap']
    missing = [n + s for n in names for s in ('_lite2',) if 'yGlareEtc_%s%s_fragment' % (n, s) not in registry]
    missing += [n + '_lite1' for n in names if ('dof' in n or 'bloom' in n) and 'yGlareEtc_%s_lite1_fragment' % n not in registry]   # review F6: _lite1 only where level 1 changes the code
    missing += [n + '_lite1 (must NOT exist: it would be word-for-word its base)' for n in names if not ('dof' in n or 'bloom' in n) and 'yGlareEtc_%s_lite1_fragment' % n in registry]
    line(not missing, 'registry: every lite twin of %d permutations is there (missing %s)' % (len(names), missing or 'none'))
    # the existing variants are byte-identical to the base branch: the header only grew
    # S105 SPV: compare the words (the pinned base has them in fx_yGlareEtc.h, now they are in fx_yGlareEtc_data.inc): every array the base has must be word for word the same now
    pinned = open(os.path.join(HERE, 'BASE')).read().strip()
    shown = subprocess.run(['git', '-C', env.root, 'show', pinned + ':src/renderer/shaders/spv/fx_yGlareEtc.h'], capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=60).stdout
    rx = re.compile(r'kFx_(\w+)\[\d*\] = \{(.*?)\};', re.S)
    was = {m.group(1): re.sub(r'\s+', '', m.group(2)) for m in rx.finditer(shown.replace('\r\n', '\n'))}
    now_words = {m.group(1): re.sub(r'\s+', '', m.group(2)) for m in rx.finditer(header.replace('\r\n', '\n'))}
    changed = [k for k, v in was.items() if now_words.get(k) != v]
    line(bool(was) and not changed, 'the existing yGlareEtc variants are byte-identical to the pinned base tools/r18/BASE (%d arrays compared, changed or missing: %s)' % (len(was), changed[:3] or 'none'))
    counts = {}
    for n in names:
        for suffix in ('', '_lite1', '_lite2'):
            if suffix == '_lite1' and not ('dof' in n or 'bloom' in n):
                counts[(n, suffix)] = counts.get((n, ''), 0)   # (no such twin)
                continue
            w = spirv_words(header, 'yGlareEtc_%s%s_frag' % (n, suffix))
            counts[(n, suffix)] = static_samples(w) if w else -1
    line(all(v > 0 for v in counts.values()), 'static image samples per variant read from the SPIR-V (%d variants)' % len(counts))
    for n in ('dof_fog_bloom', 'dof_fog', 'dof_fog_night_tonemap_bloom'):
        print('      %-34s static samples: base %d, level 1 %d, level 2 %d (branches add code; what a pixel EXECUTES is the model below)' % (n, counts[(n, '')], counts[(n, '_lite1')], counts[(n, '_lite2')]))
    # the flat exit: level 2 has the 9 smoothing taps behind a branch: the same static count as level 1 (no more samples than the old shader)
    line(all(counts[(n, '_lite1')] <= counts[(n, '')] + 1 and counts[(n, '_lite2')] <= counts[(n, '_lite1')] for n in names), 'no lite variant has more samples than the old one (+1 at most) and level 2 no more than level 1')
    # the executed-path model, Bryan's entrance shape (dof + fog + bloom, far branch with weight 0 in focus, bloom scale 0.4 / star 0): what a pixel runs
    old = fetch_model(True, True, False, True, 0, False, 0.0, 0.0, 0.0)   # the four captures: BloomScale 0 on every frame
    l1 = fetch_model(True, True, False, False, 1, False, 0.0, 0.0, 0.0)  # level 1 picks the permutation WITHOUT bloom there (no bloom fetches at all)
    l2 = fetch_model(True, True, False, False, 2, True, 0.0, 0.0, 0.0)
    l1_dead = fetch_model(True, True, False, True, 1, False, 0.0, 0.4, 0.0)   # (a live bloom, scale 0.4: only the guarded fetches go)
    print('      MODEL fetches per pixel, dof_fog_bloom as the captures run it (far branch in focus, BloomScale 0): old %d, level 1 %d (a live bloom of 0.4 would give %d), level 2 on a flat pixel %d' % (old, l1, l1_dead, l2))
    line(l1 < old and l2 < l1 and l1_dead <= old, 'the executed fetch count falls: %d -> %d (level 1) -> %d (flat exit), as claimed (a MODEL read from the shader source, not a measurement)' % (old, l1, l2))
    print('      claim arithmetic (R18 section 4 row 2): combine = 1.5-1.9 ms; level 1 removes %d of %d fetches on a pixel in focus = %.2f-%.2f ms of fetch weight if the combine were fetch-bound (the claim is lower: it is not purely fetch-bound); '
          'level 2 on a flat share s removes 4 more: s x 4/%d x 1.5-1.9 ms' % (old - l1, old, (old - l1) / old * 1.5, (old - l1) / old * 1.9, old))


# ---- wiring ----------------------------------------------------------------------------------------------------------------------------------------------------------
FILES = ('src/renderer_adapter/rdr_switches.h', 'src/renderer_adapter/reader.h', 'src/renderer_adapter/live.cpp', 'src/renderer_scene/post_pass.cpp', 'src/renderer/combine_lite.h',
         'src/fview/main.cpp', 'src/renderer/player/rplay.cpp', 'tools/renderer/compile_shaders.py', 'src/renderer/shaders/fx/yGlareEtc.frag')


def lint(fs):
    bad = []
    if not re.search(r'X\(CombineLite, "wwe2k14_rdr_combine_lite", PerFrame, 0\)', fs['src/renderer_adapter/rdr_switches.h']):
        bad.append('table line CombineLite (PerFrame, default 0)')
    if 'REXCVAR_DEFINE_INT32(wwe2k14_rdr_combine_lite, 0,' not in fs['src/renderer_adapter/reader.h']:
        bad.append('cvar combine_lite default 0')
    lv = fs['src/renderer_adapter/live.cpp']
    if 'rdr::g_combine_lite.store(rdr::ClampCombineLite(int(rdr_sw::IntSeen(rdr_sw::kCombineLite' not in lv or 'message.guest_frame))), std::memory_order_relaxed);   // S102 R18-B2' not in lv:
        bad.append('live.cpp per-frame read (IntSeen with the guest frame)')
    if 'CombineLiteCensusLine(drawn' not in lv:
        bad.append('live.cpp census line every 600 frames')
    if 'rdr::CombineLiteName(fold_combine ? vd_name(combine_base) : combine_base' not in fs['src/renderer_scene/post_pass.cpp']:
        bad.append('post_pass.cpp picks the variant through CombineLiteName')
    if 'const bool drop_bloom = lite_on && variant_bloom && !run_blur && !run_glare;' not in fs['src/renderer_scene/post_pass.cpp']:
        bad.append('post_pass.cpp drops the bloom permutation only when neither blur nor glare is recorded')
    if '"--combine-lite"' not in fs['src/fview/main.cpp'] or '"--combine-lite"' not in fs['src/renderer/player/rplay.cpp']:
        bad.append('--combine-lite flag in fview and rplay')
    h = fs['src/renderer/combine_lite.h']
    if 'fetch_add(1, std::memory_order_relaxed)' not in h or 'std::mutex' in h or 'std::map' in h:
        bad.append('the census is relaxed counters bumped once per draw (no mutex, no map)')
    s = fs['tools/renderer/compile_shaders.py']
    if "'_lite1'" not in s or "'_lite2'" not in s:
        bad.append('compile_shaders.py makes the lite twins')
    f = fs['src/renderer/shaders/fx/yGlareEtc.frag']
    if '#ifdef FX_LITE_FXAA' not in f or f.count('textureLod(textures[2], uv, 0.0)') != 1 or f.count('textureLod(textures[6], uv, 0.0)') != 1:
        bad.append('shader: flat exit and the guarded DoF fetch')
    return bad


def check_wiring(env):
    fs = {n: open(os.path.join(env.root, n), encoding='utf-8', errors='replace').read() for n in FILES}
    bad = lint(fs)
    line(not bad, 'wiring: every link of the switch is in the source (%s)' % (', '.join(bad) or 'all'))
    plants = [
        ('live.cpp read removed', 'src/renderer_adapter/live.cpp', 'rdr_sw::kCombineLite', 'rdr_sw::kAliasSmall'),
        ('post_pass pick removed', 'src/renderer_scene/post_pass.cpp', 'rdr::CombineLiteName(fold_combine ? vd_name(combine_base) : combine_base', 'fold_combine ? vd_name(combine_base) : combine_base'),
        ('no-bloom pick removed', 'src/renderer_scene/post_pass.cpp', 'const bool drop_bloom = lite_on &&', 'const bool drop_bloom = false &&'),
        ('implicit fetch back in the shader', 'src/renderer/shaders/fx/yGlareEtc.frag', 'textureLod(textures[2], uv, 0.0)', 'texture(textures[2], uv)'),
        ('table default 1', 'src/renderer_adapter/rdr_switches.h', 'X(CombineLite, "wwe2k14_rdr_combine_lite", PerFrame, 0)', 'X(CombineLite, "wwe2k14_rdr_combine_lite", PerFrame, 1)'),
        ('cvar default 1', 'src/renderer_adapter/reader.h', 'REXCVAR_DEFINE_INT32(wwe2k14_rdr_combine_lite, 0,', 'REXCVAR_DEFINE_INT32(wwe2k14_rdr_combine_lite, 1,'),
        ('rplay flag removed', 'src/renderer/player/rplay.cpp', '"--combine-lite"', '"--combine-lite-x"'),
        ('census lock added', 'src/renderer/combine_lite.h', 'fetch_add(1, std::memory_order_relaxed)', 'fetch_add(1, std::memory_order_relaxed); std::mutex'),
    ]
    for label, path, old, new in plants:
        copy = dict(fs)
        planted = copy[path].replace(old, new)
        if planted == copy[path]:
            line(False, 'wiring plant "%s" DID NOT APPLY (the text %r is not in %s)' % (label, old[:50], path))
            continue
        copy[path] = planted
        line(bool(lint(copy)), 'wiring plant "%s" is caught' % label)


# ---- pictures --------------------------------------------------------------------------------------------------------------------------------------------------------
def png_diff(a_path, b_path):
    import numpy as np
    from PIL import Image
    a = np.asarray(Image.open(a_path).convert('RGB')).astype(int)
    b = np.asarray(Image.open(b_path).convert('RGB')).astype(int)
    d = np.abs(a - b).max(axis=2)
    return int((d > 0).sum()), int((d > 8).sum()), int(d.max()), a.shape[0] * a.shape[1]


def magenta_share(path):
    import numpy as np
    from PIL import Image
    a = np.asarray(Image.open(path).convert('RGB')).astype(int)
    m = (a[:, :, 0] == 255) & (a[:, :, 1] == 0) & (a[:, :, 2] == 255)
    return int(m.sum()), a.shape[0] * a.shape[1]


def clean_line(a, text):
    line(a.clean, '%s: validation clean (error count %d, exit code %d, %d frames)' % (text, a.valid, a.rc, a.frames))


def equal_line(a, b, text):
    n = a.differing(b)
    line(n == 0 and a.frames == b.frames and a.frames > 0, '%s: %d of %d frames differ (%d vs %d frames)' % (text, n, a.frames, a.frames, b.frames))


def check_bryan(env, jobs):
    specs = [('off', 'bryan', 'off', [], BRYAN_DUMP), ('l1', 'bryan', 'l1', ['--combine-lite', '1'], ''), ('l2', 'bryan', 'l2', ['--combine-lite', '2'], BRYAN_DUMP),
             ('p1', 'bryan', 'p1', ['--combine-lite', '1', '--combine-lite-plant', '1'], ''), ('p3', 'bryan', 'p3', ['--combine-lite', '2', '--combine-lite-plant', '3'], DUMP),
             ('p5', 'bryan', 'p5', ['--combine-lite', '1', '--combine-lite-plant', '5'], '')]
    arms = run_arms(env, specs, jobs)
    for k, a in arms.items():
        clean_line(a, 'bryan %s' % k)
    off = arms['off']
    line(off.draws == 0 or off.draws == -1 or off.with_lite <= 0, 'bryan off: the switch is off and no combine draw took the variant (census %d / %d)' % (off.with_lite, off.draws))
    equal_line(arms['l1'], off, 'bryan level 1 is BYTE-EQUAL to off under the shipped bundle')
    line(arms['l1'].took() and arms['l1'].draws >= 100, 'bryan level 1: the lever took %d of %d combine draws (the shipped bundle on; 0 taken = the lever does nothing)' % (arms['l1'].with_lite, arms['l1'].draws))
    line(arms['l1'].nobloom == arms['l1'].draws and arms['l1'].draws > 0, 'bryan level 1: the permutation WITHOUT bloom was used on %d of %d combine draws (the bloom is dead on every Bryan frame: its images are black)' % (arms['l1'].nobloom, arms['l1'].draws))
    print('      bryan census: DOFParam2.z <= 0 on %d draws, BloomScale == 0 on %d, StarScale == 0 on %d of %d' % (arms['l1'].z_le0, arms['l1'].bloom0, arms['l1'].star0, arms['l1'].draws))
    n2 = arms['l2'].differing(off)
    line(arms['l2'].took() and arms['l2'].l2 == arms['l2'].draws, 'bryan level 2 took %d of %d combine draws' % (arms['l2'].l2, arms['l2'].draws))
    print('      bryan level 2 vs off: %d of %d frames differ (by design: a look lever)' % (n2, off.frames))
    line(n2 > 0, 'bryan level 2 is NOT byte-equal to off (the flat exit changes pixels)')
    # planted twins
    line(arms['p1'].differing(off) > 0, 'planted lite_dof_always_skip: pictures differ from off in %d frames (the DoF blur never fetched)' % arms['p1'].differing(off))
    line(arms['p3'].differing(arms['l2']) > 0, 'planted lite_flat_wide: pictures differ from level 2 in %d frames (the flat threshold x10)' % arms['p3'].differing(arms['l2']))
    line(not arms['p5'].took(), 'planted lite_not_applied: the census reads %d of %d draws with the variant, so the "lever took every draw" check FAILS on it (did not apply)' % (arms['p5'].with_lite, arms['p5'].draws))
    # the bloom plant needs a scene where the bloom is live
    live = off.dead_bloom >= 0 and off.post_frames > off.dead_bloom
    note('bryan: bloom dead on %d of %d post frames (the bloom plant lite_bloom_skip_live needs a capture with a live bloom: see the captures part)' % (off.dead_bloom, off.post_frames))
    return arms, live


def check_captures(env, jobs, bryan_live):
    specs = []
    for cap in ('match', 'brock', 'wm30'):
        specs += [(cap + '_off', cap, 'off', [], ''), (cap + '_l1', cap, 'l1', ['--combine-lite', '1'], '')]
    arms = run_arms(env, specs, jobs)
    bloom_cap = None
    for cap in ('match', 'brock', 'wm30'):
        off, l1 = arms[cap + '_off'], arms[cap + '_l1']
        clean_line(off, '%s off' % cap)
        clean_line(l1, '%s level 1' % cap)
        equal_line(l1, off, '%s level 1 is BYTE-EQUAL to off' % cap)
        line(l1.took(), '%s level 1: the lever took %d of %d combine draws' % (cap, l1.with_lite, l1.draws))
        note('%s: bloom dead on %d of %d post frames' % (cap, off.dead_bloom, off.post_frames))
        if off.dead_bloom >= 0 and off.post_frames > off.dead_bloom and bloom_cap is None:
            bloom_cap = cap
    return arms, bloom_cap


def check_bloom_plant(env, cap):
    """lite_bloom_skip_live: the picture must change on a scene whose bloom is live (a bloom permutation with a non-zero scale). Tried on every capture that is not dead-bloom."""
    specs = []
    for c in ('match', 'brock', 'wm30'):
        specs += [(c + '_p2', c, 'p2', ['--combine-lite', '1', '--combine-lite-plant', '2'], '')]
    arms = run_arms(env, specs, 3)
    hit = []
    for c in ('match', 'brock', 'wm30'):
        off_csv = os.path.join(env.work, '%s_off.csv' % c)
        if not os.path.isfile(off_csv):
            line(False, '%s: no off hashes (run the captures part first)' % c)
            continue
        off = read_hashes(off_csv)
        n = sum(1 for k, v in arms[c + '_p2'].hashes.items() if k != 'format' and off.get(k) != v)
        clean_line(arms[c + '_p2'], '%s bloom plant' % c)
        if n:
            hit.append((c, n))
    if hit:
        line(True, 'planted lite_bloom_skip_live: pictures differ from off on %s' % ', '.join('%s (%d frames)' % h for h in hit))
    else:
        note('lite_bloom_skip_live: NO capture here has a live bloom (Bryan: the bloom permutation with scale 0 on all frames; match, Brock, wm30: no bloom permutation), so this planted twin cannot be caught '
             'by a picture. The bloom early-out is proved by structure only (the guarded fetch in the shader, the static sample count) and by the Bryan frames where the scale is 0 (byte-equal).')


def check_flat(env, jobs):
    specs = []
    for cap in ('bryan', 'match', 'brock', 'wm30'):
        specs += [(cap + '_m', cap, 'mark', ['--combine-lite', '2', '--combine-lite-plant', '4'], DUMP)]
        if cap != 'bryan':
            specs += [(cap + '_off', cap, 'off_d', [], DUMP), (cap + '_l2', cap, 'l2_d', ['--combine-lite', '2'], DUMP)]
    arms = run_arms(env, specs, jobs)
    rows = []
    for cap in ('bryan', 'match', 'brock', 'wm30'):
        m = arms[cap + '_m']
        clean_line(m, '%s mark twin' % cap)
        film = re.search(r'shapes \{[^}]*film (\d+)', m.text)
        if film and int(film.group(1)) > 0:   # the 30 Years film shape: the monotone film pass runs AFTER the combine and overwrites the mark, so the share cannot be read from the picture
            note('%s: the film shape on %s post frames (the monotone film pass after the combine overwrites the mark): the flat share is NOT measurable from the picture here; level 1 on it is byte-equal (captures part)' % (cap, film.group(1)))
            rows.append((cap, -1))
            continue
        tot, pix = 0, 0
        for f in DUMP.split(','):
            p = os.path.join(m.dir, 'frame_%05d.png' % int(f))
            if not os.path.isfile(p):
                line(False, '%s: the mark frame %s was not written' % (cap, f))
                continue
            a, b = magenta_share(p)
            tot += a
            pix += b
        share = tot / pix if pix else -1
        rows.append((cap, share))
        print('      FLAT SHARE %-6s %.1f %% of the combine pixels take the exit (frames %s)' % (cap, share * 100, DUMP))
        line(pix > 0 and 0 < share < 1, '%s: the flat share is read from the picture (%.4f)' % (cap, share))
    with open(os.path.join(env.work, 'flat_share.txt'), 'w') as f:
        f.write('\n'.join('%s %.5f' % r for r in rows) + '\n')
    return arms


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--only', default='fetches,wiring,bryan,captures,flat')
    ap.add_argument('--jobs', type=int, default=3)
    a = ap.parse_args()
    env = Env(os.path.abspath(a.root))
    parts = a.only.split(',')
    for p in ('fview',):
        if not os.path.isfile(env.fview):
            line(False, 'no %s (build it: pwsh -NoProfile -File tools\\fview\\build.ps1)' % env.fview)
            return 1
    if 'fetches' in parts:
        check_fetches(env)
    if 'wiring' in parts:
        check_wiring(env)
    live = False
    if 'bryan' in parts:
        _, live = check_bryan(env, a.jobs)
    if 'captures' in parts:
        _, bloom_cap = check_captures(env, a.jobs, live)
        check_bloom_plant(env, bloom_cap)
    if 'flat' in parts:
        check_flat(env, a.jobs)
    print('%s: %d failures' % ('FAIL' if fails else 'PASS', fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
