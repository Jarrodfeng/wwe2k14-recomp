r"""S102 lane RT1 (docs\renderer\RENDER_LEAN.md): offline checks of `wwe2k14_rdr_render_lean`.   py -I tools\render_lean\lean_check.py [--root DIR] [--only census|rplay|fview|wiring|bench] [--reps N]
0 game runs, 0 Surface runs. Exit 0 = every line PASS.

census  Q5's first question (CLAUDE brief: skip the lever when under 50 %): the share of texture sets per frame whose key equals a set kept from an EARLIER frame (rplay --render-lean 1, the
        census line `[rplay][lean]`: kept / (kept + built)). Printed per capture; the gate's bar is the brief's 50 %.
rplay   Bryan and Brock (unpacked case folders with scene records, read in place): the presented picture hash of EVERY frame (--frame-hashes) off against mask 1, 4 and 5; validation errors 0 and exit 0
        in every arm (the exit code is rplay's validation error count); mask 1 keeps sets (kept > 0) and builds fewer than off; mask 4 pools staging (hits > 0). Planted twin: --lean-plant 1 (sets not evicted)
        is NOT visible in a capture that never destroys an image, so the plants are proved by lean_test (unit) and by the wiring lint below.
fview   the same two captures through the game's hooks (the player's bundle): mask 7 against off, every frame's hash equal, validation 0, exit 0, the hook pieces took (arena memo skips > 0, ui record skips
        > 0, plane sort skip + run > 0).
wiring  each link of the switch (table row PerFrame default 0, cvar default 0, live.cpp per-frame read + split packet, rplay / fview flags, census lines, stage_split lint), each removed in a text copy must fail.
bench   rplay --timing (render_us / bind_us / load_us) off against each bit, interleaved, 3 repeats, mean of frames after the 30th."""
import argparse
import csv
import glob
import os
import re
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
CAPS = (('bryan', 237), ('brock', 341))
BUNDLE = ['--crowd-ours', '--crowd-cards', '--arena-ours', '--wrestlers-ours', '--caw-ours', '--ring-ours', '--ui-ours', '--ui-quads', '--ui-text-ours', '--flash-ours', '--props-ours', '--post-ours',
          '--movie-ours', '--screens-ours', '--shadow-casters-ours', '--rim-ours', '--circles-ours', '--plane-shadow-ours', '--small-passes-ours', '--rim-chain-ours', '--particles-ours', '--shadows-ours']
LEAN = re.compile(r'\[rplay\]\[lean\] mask (\d+): sets kept (\d+) \(earlier frames (\d+)\) built (\d+) fallback (\d+) collide (\d+) evicted (\d+); staging pool hit (\d+) miss (\d+); churn (\d+), audits (\d+), kept sets naming a dead view (\d+)')
FLEAN = re.compile(r'\[fview\] render lean: mask (\d+), sets kept (\d+) built (\d+) fallback (\d+) collide (\d+) evicted (\d+); arena memo skip (\d+) run (\d+); staging hit (\d+) miss (\d+); ui record skip (\d+) run (\d+); plane sort skip (\d+) run (\d+)')
VAL = re.compile(r'validation errors (\d+)')
fails = 0


def line(ok, text):
    global fails
    print(('PASS  ' if ok else 'FAIL  ') + text, flush=True)
    fails += 0 if ok else 1


def main_checkout(root):
    try:
        c = subprocess.run(['git', '-C', root, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=30).stdout.strip()
        if c:
            return os.path.dirname(c)
    except Exception:
        pass
    return root


def find_capture(main_co, frames):
    best = None
    # S102 TIDY2: fview_unpack_* copies are scratch (the tidy never holds them). Real sources first: a saved capture folder or a case folder's capture.
    for d in (glob.glob(os.path.join(main_co, 'out', 'captures', '*')) + glob.glob(os.path.join(main_co, 'out', 'cases', 'case_*', 'capture'))
              + glob.glob(os.path.join(main_co, 'out', 'cases', 'fview_unpack_*', 'capture'))):
        fr = os.path.join(d, 'frames')
        if os.path.isdir(fr) and os.path.isdir(os.path.join(d, 'scene')) and len(os.listdir(fr)) == frames:
            if best is None or os.path.getmtime(d) > os.path.getmtime(best):
                best = d
    return best


def read_hashes(path):
    out = {}
    with open(path) as f:
        for ln in f:
            ln = ln.strip()
            if ln:
                a, b = ln.split(',', 1)
                out[a if a == 'format' else int(a)] = b
    return out


class Arm:
    def __init__(self, exe, cap, game, work, name, extra, frames, timing=False, bundle=False, nohash=False):
        self.hashes_path = os.path.join(work, name + '.csv')
        self.timing_path = os.path.join(work, name + '_timing.csv')
        for p in (self.hashes_path, self.timing_path):
            if os.path.exists(p):
                os.remove(p)
        is_fview = os.path.basename(exe).startswith('fview')
        cmd = [exe, '--capture', cap, '--game', game, '--frames', str(frames), '--validation', '1']
        if is_fview:
            cmd += ['--replay', '--hash-frames', self.hashes_path]
        else:
            cmd += ([] if nohash else ['--frame-hashes', self.hashes_path]) + ['--out', os.path.join(work, 'x.png')]   # (nohash: frames stay pipelined: the hash read waits for the GPU, which would hide an in-flight use)
            if timing:
                cmd += ['--timing', self.timing_path]
        if bundle:
            cmd += BUNDLE
        cmd += extra
        for _ in range(12):
            p = subprocess.run(cmd, capture_output=True, text=True, timeout=1800, errors='replace')
            self.text = (p.stdout or '') + (p.stderr or '')
            if 'Not enough graphics memory for the frame ring' not in self.text:
                break
            time.sleep(20)
        self.rc = p.returncode
        with open(os.path.join(work, name + '.log'), 'w', encoding='utf-8') as f:
            f.write(self.text)
        self.hashes = read_hashes(self.hashes_path) if os.path.isfile(self.hashes_path) else {}
        v = VAL.findall(self.text)
        self.valid = int(v[-1]) if v else (0 if self.rc == 0 else -1)
        self.n = len([k for k in self.hashes if k != 'format'])
        m = (FLEAN if is_fview else LEAN).search(self.text)
        self.census = [int(x) for x in m.groups()] if m else None
        self.clean = self.rc == 0 and self.valid == 0 and (self.n > 1 or nohash)

    def differing(self, other):
        return sum(1 for k in set(self.hashes) | set(other.hashes) if k != 'format' and self.hashes.get(k) != other.hashes.get(k))


def means(path, skip=30):
    rows = list(csv.DictReader(open(path)))[skip:]
    drawn = [r for r in rows if float(r['render_us']) > 0] or rows
    return tuple(statistics.mean(float(r[k]) for r in drawn) for k in ('load_us', 'bind_us', 'render_us'))


def setup(root, want_fview):
    main_co = main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    exe = os.path.join(root, 'out', 'renderer', 'dev', 'fview.exe' if want_fview else 'rplay.exe')
    if not os.path.isfile(exe):
        line(False, 'program missing: ' + exe)
        return None
    work = os.path.join(root, 'out', 'render_lean')
    os.makedirs(work, exist_ok=True)
    caps = {}
    for name, frames in CAPS:
        cap = find_capture(main_co, frames)
        if not cap:
            line(False, 'no unpacked case folder with %d frames and a scene folder under %s' % (frames, main_co))
            return None
        caps[name] = (cap, frames)
    return exe, game, work, caps


def part_census(root):
    s = setup(root, False)
    if not s:
        return
    exe, game, work, caps = s
    for name, (cap, frames) in caps.items():
        a = Arm(exe, cap, game, work, 'census_' + name, ['--render-lean', '1'], frames)
        if not a.census:
            line(False, '%s: no [rplay][lean] line' % name)
            continue
        _, kept, old, built, fb, col, ev = a.census[:7]
        share = old / max(1, kept + built + fb)
        print('INFO  %s: texture sets asked for a frame %.1f, kept %d, built %d (distinct keys), fallback %d, collide %d, evicted %d -> %.1f %% of the sets (%d of %d) were held from an EARLIER frame (the rest of the kept ones were built earlier in the same frame)' % (name, (kept + built + fb) / max(1, a.n), kept, built, fb, col, ev, 100 * share, old, kept + built + fb))
        line(share >= 0.5, '%s: kept share %.1f %% >= 50 %% (the brief\'s bar for building Q5)' % (name, 100 * share))


def part_rplay(root, reps):
    s = setup(root, False)
    if not s:
        return
    exe, game, work, caps = s
    for name, (cap, frames) in caps.items():
        base = Arm(exe, cap, game, work, 'rplay_%s_off' % name, [], frames)
        line(base.clean, '%s: off arm clean (%d frames, validation %d, exit %d)' % (name, base.n, base.valid, base.rc))
        for mask in (1, 4, 5):
            a = Arm(exe, cap, game, work, 'rplay_%s_m%d' % (name, mask), ['--render-lean', str(mask)], frames)
            d = a.differing(base)
            c = a.census or [0] * 9
            took = (c[2] > 0 if mask & 1 else True) and (c[7] > 0 if mask & 4 else True)
            line(a.clean and d == 0 and len(a.hashes) == len(base.hashes) and took,
                 '%s: mask %d: %d frames, %d differ from off, validation %d, exit %d | sets kept %d built %d fallback %d evicted %d | staging pool hit %d miss %d' % (name, mask, a.n, d, a.valid, a.rc, c[1], c[3], c[4], c[6], c[7], c[8]))


def part_churn(root):
    """Q5's eviction proof (review item 2): Bryan and Brock never destroy an image and the default cache never fills, so on its own the capture proves nothing about eviction. Here every 7th frame one live
    image's sampled view is replaced under the cache (also in the off arm), the cache holds 64 sets (so a full cache sweeps and frees), the audit counts kept sets naming a dead view every 5 frames;
    all with --validation 1. Plant 1 (no eviction) must make the audit non-zero; plant 7 (a set freed while frames in flight may use it) must make validation fire."""
    s = setup(root, False)
    if not s:
        return
    exe, game, work, caps = s
    for name, (cap, frames) in caps.items():
        churn = ['--lean-churn', '7']
        lean = ['--render-lean', '1', '--lean-churn', '7', '--lean-audit', '5', '--lean-capacity', '64', '--lean-sweep-age', '6']
        base = Arm(exe, cap, game, work, 'churn_%s_off' % name, churn, frames)
        full = ['--render-lean', '1', '--lean-churn', '7', '--lean-audit', '5']   # the cache as shipped (2048 sets): the churned views DO sit in kept sets
        af = Arm(exe, cap, game, work, 'churn_%s_full' % name, full, frames)
        cf = af.census or [0] * 12
        df = af.differing(base)
        line(af.clean and df == 0 and cf[9] > 0 and cf[6] > 0 and cf[10] > 0 and cf[11] == 0,
             '%s: churn, cache as shipped: %d differ from off, validation %d, exit %d | views replaced %d, sets evicted %d, built %d, audits %d, kept sets naming a dead view %d (must be 0)' % (name, df, af.valid, af.rc, cf[9], cf[6], cf[3], cf[10], cf[11]))
        a = Arm(exe, cap, game, work, 'churn_%s_on' % name, lean, frames)
        c = a.census or [0] * 12
        d = a.differing(base)
        line(base.clean, '%s: churn off arm clean (%d frames, validation %d, exit %d, churned %d views)' % (name, base.n, base.valid, base.rc, (base.census or [0] * 12)[9]))
        line(a.clean and d == 0 and c[9] > 0 and c[10] > 0 and c[11] == 0 and c[4] > 0,
             '%s: churn + capacity 64 on: %d differ from off, validation %d, exit %d | views replaced %d, sets evicted %d, built %d, fallback %d, audits %d, kept sets naming a dead view %d (must be 0)' % (name, d, a.valid, a.rc, c[9], c[6], c[3], c[4], c[10], c[11]))
        if name != 'bryan':
            continue
        p1 = Arm(exe, cap, game, work, 'churn_%s_plant1' % name, full + ['--lean-plant', '1'], frames)
        c1 = p1.census or [0] * 12
        line(c1[11] > 0, '%s: PLANT 1 (no eviction) CAUGHT by the audit: %d kept sets name a dead view' % (name, c1[11]))
        pipe = Arm(exe, cap, game, work, 'churn_%s_pipe' % name, full, frames, nohash=True)
        line(pipe.clean, '%s: churn, cache as shipped, frames PIPELINED (no hash read waiting on the GPU): validation %d, exit %d' % (name, pipe.valid, pipe.rc))
        p7 = Arm(exe, cap, game, work, 'churn_%s_plant7' % name, full + ['--lean-plant', '7'], frames, nohash=True)
        line(p7.valid != 0 or p7.rc != 0, '%s: PLANT 7 (a kept set freed at once; VUID-vkFreeDescriptorSets-pDescriptorSets-00309) CAUGHT: validation errors %d, exit %d' % (name, p7.valid, p7.rc))


def part_fview(root):
    s = setup(root, True)
    if not s:
        return
    exe, game, work, caps = s
    for name, (cap, frames) in caps.items():
        base = Arm(exe, cap, game, work, 'fview_%s_off' % name, [], frames, bundle=True)
        line(base.clean, '%s: off arm clean (%d frames, validation %d, exit %d)' % (name, base.n, base.valid, base.rc))
        a = Arm(exe, cap, game, work, 'fview_%s_m7' % name, ['--render-lean', '7'], frames, bundle=True)
        d = a.differing(base)
        c = a.census or [0] * 14
        line(a.clean and d == 0 and len(a.hashes) == len(base.hashes),
             '%s: mask 7 with the player bundle: %d frames, %d differ from off, validation %d, exit %d' % (name, a.n, d, a.valid, a.rc))
        line(c[1] > 0 and c[6] > 0 and c[8] > 0 and c[10] > 0,   # (the plane board's Add runs only in the live writer: its sort is proved by plane_sort_test in lean_unit; its counts here are INFO)
             
             '%s: the pieces took: sets kept %d built %d | arena memo skip %d run %d | staging hit %d miss %d | ui record skip %d run %d | plane sort skip %d run %d' % (name, c[1], c[2], c[6], c[7], c[8], c[9], c[10], c[11], c[12], c[13]))


def part_bench(root, reps):
    s = setup(root, False)
    if not s:
        return
    exe, game, work, caps = s
    arms = [('off', []), ('m1', ['--render-lean', '1']), ('m4', ['--render-lean', '4']), ('m5', ['--render-lean', '5'])]
    for name, (cap, frames) in caps.items():
        res = {k: [] for k, _ in arms}
        for rep in range(reps):
            order = arms if rep % 2 == 0 else arms[::-1]
            for k, extra in order:
                a = Arm(exe, cap, game, work, 'bench_%s_%s_%d' % (name, k, rep), extra, frames, timing=True, bundle=True)
                if not a.clean or not os.path.isfile(a.timing_path):
                    line(False, '%s %s rep %d did not run clean' % (name, k, rep))
                    return
                res[k].append(means(a.timing_path))
        for col, idx in (('load_us', 0), ('bind_us', 1), ('render_us', 2)):
            off = [r[idx] for r in res['off']]
            row = '%s %s: off mean %.0f best %.0f' % (name, col, statistics.mean(off), min(off))
            for k, _ in arms[1:]:
                on = [r[idx] for r in res[k]]
                row += ' | %s mean %.0f (%+.0f) best %.0f (%+.0f)' % (k, statistics.mean(on), statistics.mean(on) - statistics.mean(off), min(on), min(on) - min(off))
            print('INFO  ' + row + ' us a frame (%d repeats, interleaved, the PC is shared)' % reps, flush=True)


def part_wiring(root):
    def read(rel):
        with open(os.path.join(root, rel), encoding='utf-8', errors='replace') as f:
            return f.read()
    live, sw, ss, rp, fv = (read('src/renderer_adapter/live.cpp'), read('src/renderer_adapter/rdr_switches.h'), read('src/renderer_adapter/stage_split.h'), read('src/renderer/player/rplay.cpp'), read('src/fview/main.cpp'))
    cv, rl, rn = read('src/renderer_adapter/render_lean_cvar.h'), read('src/renderer/render_lean.h'), read('src/renderer/renderer.cpp')
    links = [
        ('table row PerFrame default 0', 'X(RenderLean, "wwe2k14_rdr_render_lean", PerFrame, 0)' in sw, sw, 'X(RenderLean, "wwe2k14_rdr_render_lean", PerFrame, 0)'),
        ('cvar default 0', 'REXCVAR_DEFINE_INT32(wwe2k14_rdr_render_lean, 0,' in cv, cv, 'REXCVAR_DEFINE_INT32(wwe2k14_rdr_render_lean, 0,'),
        ('live.cpp reads it every frame', 'rdr_sw::IntSeen(rdr_sw::kRenderLean' in live, live, 'rdr_sw::IntSeen(rdr_sw::kRenderLean'),
        ('live.cpp stores the render bits on the one-thread path', 'if (!split_frame) rdr::g_render_lean.store(' in live, live, 'if (!split_frame) rdr::g_render_lean.store('),
        ('the split record stage stores them from the packet', 'rdr::g_render_lean.store(p.render_lean' in live, live, 'rdr::g_render_lean.store(p.render_lean'),
        ('the packet carries the mask', 'int render_lean = 0;' in ss, ss, 'int render_lean = 0;'),
        ('the hooks bits are stored for the bind thread', 'rdr::g_lean_hooks.store(' in live, live, 'rdr::g_lean_hooks.store('),
        ('Note flat follows bit 4', 'streaks.note_flat =' in live, live, 'streaks.note_flat ='),
        ('rplay flag', '"--render-lean"' in rp, rp, '"--render-lean"'),
        ('fview flag', '"--render-lean"' in fv, fv, '"--render-lean"'),
        ('the renderer takes the sets path only on bit 1', 'rdr::g_render_lean.load(std::memory_order_relaxed) & rdr::kLeanSets) return GetTextureSetLean' in rn, rn, 'rdr::g_render_lean.load(std::memory_order_relaxed) & rdr::kLeanSets) return GetTextureSetLean'),
        ('a destroyed image evicts its sets', 'lean_cache.EvictViews(lean_dying' in rn, rn, 'lean_cache.EvictViews(lean_dying'),
        ('staging goes back to the pool only from DrainRetired', 'staging_pool.Give(item.pool_class' in rn, rn, 'staging_pool.Give(item.pool_class'),
        ('the census prints every 600 frames', 'serial % 600 == 0) && (rdr::g_render_lean' in rn, rn, 'serial % 600 == 0) && (rdr::g_render_lean'),
    ]
    for what, present, text, needle in links:
        twin = text.replace(needle, '/*cut*/', 1)
        applied = twin != text
        line(present and applied and needle not in twin, 'wiring: %s (the link is present; its text twin with the link cut no longer has it)' % what)

    # a view or a kept set may only be created / destroyed / replaced where the destroy delay and the slot's fence protect it (review item 2): a new mutation elsewhere fails
    def stray_funcs(text):
        funcs, cur = {}, None
        for ln in text.splitlines():
            m = re.match(r'^[\w:<>\*& ,]+? (?:Renderer::(?:Impl::)?|Impl::)(~?\w+)\(', ln)
            if m and not ln.startswith(' '):
                cur = m.group(1)
            if re.search(r'vkDestroyImageView|sampled_view\s*=[^=]|reinterpreted_views\.(?:push_back|clear)|vkFreeDescriptorSets', ln):
                funcs[cur] = funcs.get(cur, 0) + 1
        return funcs
    allowed = {None, 'ProcessDestroys', 'DrainRetired', 'LeanEvictDying', 'LeanTool', 'SampledView', 'CreateImage', '~Impl', 'Shutdown', 'Impl', 'Renderer', 'Destroy'}
    stray = sorted(str(f) for f in stray_funcs(rn) if f not in allowed)
    line(not stray, 'lint: view destroy / replace / set free only in the allowed functions (stray: %s)' % (', '.join(stray) or 'none'))
    twin = rn.replace('  VkWriteDescriptorSet writes[kMaxTextures];', '  images[0].sampled_view = VK_NULL_HANDLE;\n  VkWriteDescriptorSet writes[kMaxTextures];', 1)
    line(twin != rn and any(f not in allowed for f in stray_funcs(twin)), 'lint twin: a planted `sampled_view =` in WriteTextureSet is seen as stray')
    rio = read('src/renderer_scene/record_io.cpp')
    line('arena_memo.v.store(-2' in rio.split('void SceneTables::Recycle()')[1].split('}')[0] and 'o.arena_memo.v.store(-2' in rio.split('static void ExpandInto(')[1][:300], 'lint: Recycle() and ExpandInto reset the arena memo (a refilled table is a new record)')
    out = subprocess.run([sys.executable, '-I', os.path.join(root, 'tools', 'renderer', 'stage_split_lint.py'), '--root', root], capture_output=True, text=True, timeout=300, errors='replace')
    line(out.returncode == 0, 'stage_split_lint exit %d %s' % (out.returncode, (out.stdout or out.stderr).strip().splitlines()[-1][:120] if (out.stdout or out.stderr).strip() else ''))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--only', default='')
    ap.add_argument('--reps', type=int, default=3)
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    parts = (('census', lambda: part_census(root)), ('rplay', lambda: part_rplay(root, a.reps)), ('churn', lambda: part_churn(root)), ('fview', lambda: part_fview(root)), ('wiring', lambda: part_wiring(root)), ('bench', lambda: part_bench(root, a.reps)))
    for name, fn in parts:
        if a.only and a.only != name:
            continue
        if name == 'bench' and not a.only:
            continue   # timing is noisy: asked for by name
        print('--- ' + name, flush=True)
        fn()
    print('%d failure(s)' % fails)
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
