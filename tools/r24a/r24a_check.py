#!/usr/bin/env python3
"""S102 lane R24-A: the 1:1 present as one texelFetch (wwe2k14_rdr_present_fetch, docs/renderer/PRESENT_COPY.md). Offline, 0 game runs, 0 Surface runs, no window.

  py -I tools\\r24a\\r24a_check.py [--root DIR] [--only wiring|core|core_bryan|core_match|core_brock|twins|valid|counts]

The switch is PerFrame, default off. ON: when the picture is shown exactly 1:1 the present's fragment shader is rdr_present_fetch.frag (one texelFetch at gl_FragCoord, then the display curve), not the
filtered sample. EVERY proof runs WITH a real display curve (--display-curve-s100: the S100 table's five documented points): the live game's curve is never the identity (LIVE_ADAPTER.md), captures carry none and the
tools start from the identity, so a proof under the identity curve is a tool artefact (the first version of this lane fell into it).
Every arm is fview headless with --present-size (the present into offscreen images: the swapchain image the player would see) and --present-hash-frames (FNV-1a of that image after EVERY frame); the exit code AND
`validation errors N` are read, never "the csv exists".
wiring  each link (table PerFrame default 0, cvar default false, live.cpp per-frame read with guest_frame, frame builder, the render thread reads only the frame, the shader, the census, flags); each removed in a
        text copy must fail the lint (a twin that no longer applies fails as DID NOT APPLY).
core    bryan / match / brock, the default bundle, the real curve: the presented image's hash of every frame equal off vs on, validation 0, exit 0, taken = presented, declined 0 (0 taken = nothing for players).
twins   the curve is applied (the picture differs from the identity-curve one: the S100 gamma-table fault class, here as a harness sensitivity check); 1280x720 into 1366x768 declines (fit) and equals the draw;
        PLANT 2 (the fit test skipped) takes the fetch on that panel and the hash must DIFFER; an overlay frame at 1:1 takes the fetch, equals the draw and shows the overlay.
valid   rplay --validation 1 and 3 (synchronization) on the synthetic scene at 640x360, the fetch taken, picture equal to the draw.
counts  rplay --counts (passes, copies, barriers, pipeline binds a frame, off vs on) and the `output` GPU role off vs on.
Exit 0 only when everything holds."""
import argparse
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
CAP_ZIP = {
    'bryan': 'out/s101/runs/20261112_122700_s100_owner_bryan_entrance/case_20261112_122719_969_0_pc_run_exit.zip',
    'match': 'out/s101/runs/20261112_131008_s100_owner_match/case_20261112_131027_265_0_pc_run_exit.zip',
}
CAP_DIR = {'brock': 'out/lane86/brock/capture'}
BRYAN_COUNTS_DIR = 'out/s101/lane92/case_20261112_122719_969_0_pc_run_exit/capture'
DEFAULT3 = ['--crowd-ours', '--crowd-cards', '--shadows-ours', '--shadow-casters-ours', '--plane-shadow-ours']  # the players' default bundle (c3_check)
CURVE = ['--display-curve-s100']  # the game's display curve (never the identity live)
FRAMES = 60
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
                out[int(a)] = b
    return out


CENSUS = re.compile(r'\[(?:fview|rplay)\]\[present_fetch\] switch (\d), frames (\d+), taken (\d+), declined fit (\d+) sgsr_dlss (\d+) cap (\d+)')
PRESENTS = re.compile(r'\[(?:fview|rplay)\]\[present\] (?:size \S+: )?presents (\d+),')


class Arm:
    """One fview replay of a standard capture with the present into offscreen images of size WxH."""

    def __init__(self, exe, cap, game, work, name, extra, size='1280x720', frames=FRAMES, bundle=DEFAULT3):
        hashes = os.path.join(work, name + '.csv')
        shot = os.path.join(work, name + '.png')
        for p in (hashes, shot):
            if os.path.exists(p):
                os.remove(p)
        kind, path = cap
        cmd = [exe, '--case' if kind == 'zip' else '--capture', path, '--replay', '--frames', str(frames), '--validation', '1', '--game', game, '--present-size', size,
               '--present-hash-frames', hashes, '--present-shot', shot] + bundle + extra
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=1700, errors='replace')
        self.text = (p.stdout or '') + (p.stderr or '')
        self.rc = p.returncode
        self.shot = shot
        with open(os.path.join(work, name + '.log'), 'w', encoding='utf-8') as f:
            f.write(self.text)
        self.ok_hashes = os.path.isfile(hashes)
        self.hashes = read_hashes(hashes) if self.ok_hashes else {}
        v = re.findall(r'validation errors (\d+)', self.text)
        self.valid = int(v[-1]) if v else -1
        m = CENSUS.search(self.text)
        keys = ('switch', 'frames', 'taken', 'fit', 'sgsr', 'cap')
        self.c = dict(zip(keys, (int(x) for x in m.groups()))) if m else dict.fromkeys(keys, -1)
        m = PRESENTS.search(self.text)
        self.presents = int(m.group(1)) if m else -1
        self.clean = self.rc == 0 and self.valid == 0 and self.ok_hashes and len(self.hashes) > 1 and '0000000000000000' not in self.hashes.values()

    def differing(self, other):
        return sum(1 for k in self.hashes if self.hashes.get(k) != other.hashes.get(k))

    def declined(self):
        return self.c['fit'] + self.c['sgsr'] + self.c['cap']


def setup(root, which):
    main_co = main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    sys.path.insert(0, os.path.join(root, 'tools', 'fview'))
    import fview_exe as fe
    exe = fe.find_or_exit(root=root, kind='dev')
    work = os.path.join(root, 'out', 'r24a_check')
    os.makedirs(work, exist_ok=True)
    cap = ('zip', os.path.join(main_co, CAP_ZIP[which])) if which in CAP_ZIP else ('dir', os.path.join(main_co, CAP_DIR[which]))
    for p in (cap[1], game):
        if not os.path.exists(p):
            line(False, 'input missing: ' + p)
            return None
    return exe, cap, game, work


# ---- wiring ---------------------------------------------------------------------------------------------------------------------------------------------------------
FILES = ('src/renderer_adapter/rdr_switches.h', 'src/renderer_adapter/reader.h', 'src/renderer_adapter/live.cpp', 'src/renderer/frame_builder.h', 'src/renderer/api.h', 'src/renderer/renderer.cpp',
         'src/renderer/shaders/rdr_present_fetch.frag', 'src/fview/main.cpp', 'src/renderer/player/rplay.cpp', 'src/gfx_applied.h')


def lint(fs):
    bad = []
    if not re.search(r'X\(PresentFetch, "wwe2k14_rdr_present_fetch", PerFrame, 0\)', fs['src/renderer_adapter/rdr_switches.h']):
        bad.append('table line PresentFetch (PerFrame, default 0)')
    if 'REXCVAR_DEFINE_BOOL(wwe2k14_rdr_present_fetch, false,' not in fs['src/renderer_adapter/reader.h']:
        bad.append('cvar default false')
    if 'fb.SetPresentFetch(rdr_sw::BoolSeen(rdr_sw::kPresentFetch, false, message.guest_frame))' not in fs['src/renderer_adapter/live.cpp']:
        bad.append('live.cpp reads the switch every frame (BoolSeen with the guest frame) and hands it to the frame')
    fb = fs['src/renderer/frame_builder.h']
    if 'frame.present_fetch = present_fetch_;' not in fb or 'void SetPresentFetch(bool on)' not in fb:
        bad.append('the frame builder carries it into the Frame')
    if 'bool present_fetch = false;' not in fs['src/renderer/api.h']:
        bad.append('Frame::present_fetch')
    r = fs['src/renderer/renderer.cpp']
    if 'present_fetch_ = frame.present_fetch;' not in r:
        bad.append('the render side takes it from the Frame')
    if re.search(r'g_present_fetch\.load', r):
        bad.append('the render thread must not read the live global (renderer.cpp)')
    if 'if (present_fetch_) {' not in r:
        bad.append('off = the old program pick (the fetch is tried only when the frame asks)')
    for needle, why in (('if (dlss || sgsr) {\n      count(kPcDeclSgsrDlss);', 'sgsr / dlss'), ('} else if (out_capped) {\n      count(kPcDeclCap);', 'cap'),
                        ('if (one_to_one || plant == 2) state.program = present_fetch_program, count(kPcTaken);', 'fit (the only way the program changes)')):
        if needle not in r:
            bad.append('the decision declines on ' + why)
    if 'source.width == panel_w && source.height == panel_h && v_w == float(panel_w) && v_h == float(panel_h)' not in r or 'swapchain.quarter_turns() == 0' not in r:
        bad.append('1:1 = the picture is the panel\'s size, the fit fills it, no turn')
    if 'renderer->impl_->present_fetch_program = renderer->AddProgram(present);' not in r or 'kSpv_rdr_present_fetch_frag' not in r:
        bad.append('the fetch program is registered')
    if '[rdr][present_fetch] frames %llu, taken %llu' not in r:
        bad.append('the census line')
    sh = fs['src/renderer/shaders/rdr_present_fetch.frag']
    if 'texelFetch(source_image, ivec2(gl_FragCoord.xy), 0)' not in sh or sh.count('texelFetch(display_curve') != 3 or 'texture(source_image' in sh:
        bad.append('the shader reads one texel at gl_FragCoord and keeps the three curve fetches (no filtered sample)')
    for path in ('src/fview/main.cpp', 'src/renderer/player/rplay.cpp'):
        for flag in ('--present-fetch', '--present-fetch-plant', '--display-curve-s100', '--display-curve-file', '--overlay-plant'):
            if '"%s"' % flag not in fs[path]:
                bad.append('%s flag %s' % (os.path.basename(path), flag))
    if '"wwe2k14_rdr_present_fetch"' not in fs['src/gfx_applied.h']:
        bad.append('gfx_applied row')
    return bad


def wiring(root):
    files = {}
    for k in FILES:
        with open(os.path.join(root, k), encoding='utf-8', errors='replace', newline='') as f:
            files[k] = f.read().replace('\r\n', '\n')
    bad = lint(files)
    line(not bad, 'wiring: table (PerFrame, default 0), cvar (default false), per-frame read, the frame carries it, decisions, the shader, census line, flags ' + ('' if not bad else 'MISSING: ' + '; '.join(bad)))
    R, SH = 'src/renderer/renderer.cpp', 'src/renderer/shaders/rdr_present_fetch.frag'
    twins = [('src/renderer_adapter/rdr_switches.h', 'X(PresentFetch, "wwe2k14_rdr_present_fetch", PerFrame, 0)', 'X(PresentFetch, "wwe2k14_rdr_present_fetch", StartOnly, 0)'),
             ('src/renderer_adapter/reader.h', 'REXCVAR_DEFINE_BOOL(wwe2k14_rdr_present_fetch, false,', 'REXCVAR_DEFINE_BOOL(wwe2k14_rdr_present_fetch, true,'),
             ('src/renderer_adapter/live.cpp', 'rdr_sw::BoolSeen(rdr_sw::kPresentFetch, false, message.guest_frame)', 'rdr_sw::BoolSeen(rdr_sw::kPresentFetch, false, frame_number_)'),
             ('src/renderer/frame_builder.h', 'frame.present_fetch = present_fetch_;', ''),
             (R, 'present_fetch_ = frame.present_fetch;', 'present_fetch_ = g_present_fetch.load();'),
             (R, 'if (dlss || sgsr) {\n      count(kPcDeclSgsrDlss);', 'if (false) {\n      count(kPcDeclSgsrDlss);'),
             (R, '} else if (out_capped) {\n      count(kPcDeclCap);', '} else if (false) {\n      count(kPcDeclCap);'),
             (R, 'if (one_to_one || plant == 2) state.program', 'if (true) state.program'),
             (R, 'v_w == float(panel_w) && v_h == float(panel_h);', 'true;'),
             (R, 'swapchain.quarter_turns() == 0 && ', ''),
             (R, 'renderer->impl_->present_fetch_program = renderer->AddProgram(present);', ''),
             (SH, 'texelFetch(source_image, ivec2(gl_FragCoord.xy), 0)', 'texture(source_image, in_uv)'),
             (SH, '  out_color = vec4(texelFetch(display_curve, ivec2(index.r, 0), 0).r,', '  out_color = vec4(float(index.r) / 255.0,'),
             ('src/fview/main.cpp', '"--display-curve-s100"', '"--display-curve-s100x"'),
             ('src/renderer/player/rplay.cpp', '"--present-fetch-plant"', '"--present-fetch-plantx"')]
    for path, old, new in twins:
        if old not in files[path]:
            line(False, 'wiring twin: the text %r is not in %s (the lint is stale: DID NOT APPLY)' % (old[:48], path))
            continue
        twin = dict(files)
        twin[path] = files[path].replace(old, new, 1)
        if twin[path] == files[path]:
            line(False, 'wiring twin %r DID NOT APPLY' % old[:40])
            continue
        line(bool(lint(twin)), 'wiring twin (%s: %r -> %r) is caught' % (os.path.basename(path), old[:40].replace('\n', ' '), new[:30].replace('\n', ' ')))


# ---- replay arms ------------------------------------------------------------------------------------------------------------------------------------------------------
def pair(exe, cap, game, work, tag, extra, size='1280x720', frames=FRAMES, label=''):
    A = Arm(exe, cap, game, work, tag + '_off', CURVE + extra, size, frames)
    B = Arm(exe, cap, game, work, tag + '_on', CURVE + ['--present-fetch'] + extra, size, frames)
    d = B.differing(A)
    ok = A.clean and B.clean and d == 0 and len(A.hashes) == len(B.hashes) and B.presents > 1 and B.c['taken'] == B.presents and B.declined() == 0 and A.c['taken'] <= 0
    line(ok, '%s: %d presented frames, %d differ (hash of the swapchain image, real display curve), validation %d / %d, exit %d / %d | fetch taken %d of %d presents, declined %d (fit %d sgsr/dlss %d cap %d)' %
         (label or tag, len(B.hashes), d, A.valid, B.valid, A.rc, B.rc, B.c['taken'], B.presents, B.declined(), B.c['fit'], B.c['sgsr'], B.c['cap']))
    return A, B


def core(root, which):
    s = setup(root, which)
    if s:
        exe, cap, game, work = s
        pair(exe, cap, game, work, 'core_' + which, [], label='%s (default bundle, 1280x720)' % which)


def twins(root):
    s = setup(root, 'bryan')
    if not s:
        return
    exe, cap, game, work = s
    F = 12
    ident = Arm(exe, cap, game, work, 'tw_ident', [], '1280x720', F)                       # the identity curve (what the tools used to prove under)
    ref = Arm(exe, cap, game, work, 'tw_ref', CURVE, '1280x720', F)                        # the real curve, the draw
    line(ident.clean and ref.clean and ref.differing(ident) == len(ref.hashes) > 0,
         'sensitivity: the real display curve is applied by the draw (%d of %d frames differ from the identity-curve picture): a present that dropped the curve (the S100 gamma-table fault class) would be caught by every arm here' %
         (ref.differing(ident), len(ref.hashes)))
    on = Arm(exe, cap, game, work, 'tw_on', CURVE + ['--present-fetch'], '1280x720', F)
    line(on.clean and on.differing(ref) == 0 and on.c['taken'] == on.presents > 1, 'fetch under the real curve equals the draw (%d frames differ, taken %d of %d)' % (on.differing(ref), on.c['taken'], on.presents))
    off2, on2 = Arm(exe, cap, game, work, 'tw_fit_off', CURVE, '1366x768', F), Arm(exe, cap, game, work, 'tw_fit_on', CURVE + ['--present-fetch'], '1366x768', F)
    line(off2.clean and on2.clean and on2.differing(off2) == 0 and on2.c['taken'] == 0 and (on2.c['fit'] + on2.c['sgsr']) == on2.presents > 1,
         'twin fit: a 1280x720 picture into 1366x768 declines (taken %d, declined fit %d sgsr %d of %d presents; %d frames differ off vs on)' % (on2.c['taken'], on2.c['fit'], on2.c['sgsr'], on2.presents, on2.differing(off2)))
    p2 = Arm(exe, cap, game, work, 'tw_fit_p2', CURVE + ['--present-fetch', '--present-fetch-plant', '2'], '1366x768', F)
    line(p2.rc == 0 and p2.valid == 0 and p2.c['taken'] > 0 and p2.differing(off2) > 0, 'PLANT 2 (the fit test skipped: the fetch shader on a panel that is not 1:1) is applied (taken %d) and CAUGHT (%d frames differ)' % (p2.c['taken'], p2.differing(off2)))
    off3, on3 = Arm(exe, cap, game, work, 'tw_ov_off', CURVE + ['--overlay-plant'], '1280x720', F), Arm(exe, cap, game, work, 'tw_ov_on', CURVE + ['--overlay-plant', '--present-fetch'], '1280x720', F)
    line(off3.clean and on3.clean and on3.differing(off3) == 0 and on3.c['taken'] == on3.presents > 1 and off3.differing(ref) > 0,
         'twin overlay: an overlay frame at 1:1 takes the fetch (%d of %d) and still shows the overlay: equals the draw (%d differ), and differs from the no-overlay picture (%d)' % (on3.c['taken'], on3.presents, on3.differing(off3), off3.differing(ref)))


def rplay_run(rplay, work, name, extra, frames=40, capdir=None, game=None, size='1280x720'):
    cmd = [rplay] + (['--capture', capdir, '--game', game] if capdir else []) + ['--frames', str(frames), '--present-size', size] + extra
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=1700, errors='replace')
    text = (p.stdout or '') + (p.stderr or '')
    with open(os.path.join(work, name + '.log'), 'w', encoding='utf-8') as f:
        f.write(text)
    v = re.findall(r'validation errors (\d+)', text)
    return p.returncode, (int(v[-1]) if v else -1), text


def valid(root):
    rplay = os.path.join(root, 'out', 'renderer', 'dev', 'rplay.exe')
    work = os.path.join(root, 'out', 'r24a_check')
    os.makedirs(work, exist_ok=True)
    if not os.path.isfile(rplay):
        line(False, 'rplay.exe missing: run tools\\renderer\\build.ps1')
        return
    sys.path.insert(0, HERE)
    import numpy as np
    from PIL import Image
    shots = {}
    for tag, extra in (('off', []), ('on', ['--present-fetch'])):
        for level in (1, 3):
            shot = os.path.join(work, 'valid_%s_%d.png' % (tag, level))
            rc, v, text = rplay_run(rplay, work, 'valid_%s_%d' % (tag, level), CURVE + extra + ['--validation', str(level), '--present-shot', shot], size='640x360')
            m = CENSUS.search(text)
            taken = int(m.group(3)) if m else -1
            line(rc == 0 and v == 0 and (taken > 0) == (tag == 'on'), 'rplay synthetic scene 640x360, real curve, switch %s, --validation %d: exit %d, validation errors %d, fetch taken %d' % (tag, level, rc, v, taken))
            shots[(tag, level)] = shot
    a = np.asarray(Image.open(shots[('off', 1)]).convert('RGB'), dtype=np.int16)
    b = np.asarray(Image.open(shots[('on', 1)]).convert('RGB'), dtype=np.int16)
    line(a.shape == b.shape and int(abs(a - b).max()) == 0 and a.std() > 1, 'rplay synthetic scene: the fetch picture is the draw picture (max difference %d, picture std %.1f)' % (int(abs(a - b).max()), a.std()))


def counts(root):
    main_co = main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    rplay = os.path.join(root, 'out', 'renderer', 'dev', 'rplay.exe')
    capdir = os.path.join(main_co, BRYAN_COUNTS_DIR)
    work = os.path.join(root, 'out', 'r24a_check')
    os.makedirs(work, exist_ok=True)
    if not (os.path.isfile(rplay) and os.path.isdir(capdir)):
        line(False, 'rplay or its capture is missing: %s %s' % (rplay, capdir))
        return
    rows = {}
    for tag, extra in (('off', []), ('on', ['--present-fetch'])):
        cj = os.path.join(work, 'cnt_%s.json' % tag)
        rc, v, text = rplay_run(rplay, work, 'cnt_' + tag, CURVE + extra + ['--counts', cj, '--gpu-stamps', '1', '--gpu-log', os.path.join(work, 'cnt_%s_gpu.csv' % tag)] + DEFAULT3, 100, capdir, game)
        acc = dict(copies=0, barriers=0, passes=0, binds=0, draws=0)
        n = 0
        with open(cj, encoding='utf-8') as f:
            for ln in f:
                ln = ln.strip().rstrip(',')
                if ln.startswith('{"frame"'):
                    d = json.loads(ln)
                    if d['frame'] >= 10:
                        acc['copies'] += d['total']['copies']
                        acc['barriers'] += d['total']['barriers']
                        acc['binds'] += d['total']['pipeline_binds']
                        acc['draws'] += d['total']['draws'] + d['total']['indexed_draws']
                        acc['passes'] += d['passes']
                        n += 1
        row = {k: x / max(1, n) for k, x in acc.items()}
        m = re.search(r'roles: frames \d+, mean ms a frame: .*? output ([\d.]+) ', text)
        row['output_ms'] = float(m.group(1)) if m else -1.0
        mc = CENSUS.search(text)
        row['taken'] = int(mc.group(3)) if mc else -1
        row['rc'], row['valid'] = rc, v
        rows[tag] = row
    a, b = rows['off'], rows['on']
    line(a['rc'] == 0 and b['rc'] == 0 and b['taken'] > 0 and b['passes'] <= a['passes'] + 0.01 and b['copies'] <= a['copies'] + 0.01 and b['barriers'] <= a['barriers'] + 0.01 and b['binds'] <= a['binds'] + 0.01,
         'rplay --counts (100 frames, default bundle, real curve, present 1280x720): fetch taken %d presents; passes %.2f -> %.2f, copies %.2f -> %.2f, barriers %.2f -> %.2f, pipeline binds %.2f -> %.2f' %
         (b['taken'], a['passes'], b['passes'], a['copies'], b['copies'], a['barriers'], b['barriers'], a['binds'], b['binds']))
    line(a['output_ms'] >= 0 and b['output_ms'] >= 0, "the 'output' GPU role (this PC, RTX 4090, mean ms a frame): off %.4f -> on %.4f (information; the Surface decides)" % (a['output_ms'], b['output_ms']))
    with open(os.path.join(work, 'counts_table.txt'), 'w') as f:
        for tag, c in rows.items():
            f.write('%-4s %s\n' % (tag, json.dumps(c)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--only', default='')
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    o = args.only
    if o in ('', 'wiring'):
        wiring(root)
    for which in ('bryan', 'match', 'brock'):
        if o in ('', 'core', 'core_' + which):
            core(root, which)
    if o in ('', 'twins'):
        twins(root)
    if o in ('', 'valid'):
        valid(root)
    if o in ('', 'counts'):
        counts(root)
    print('r24a_check: %d failure(s)' % fails)
    return 1 if fails else 0


sys.exit(main())
