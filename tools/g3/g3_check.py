#!/usr/bin/env python3
"""S102 lane G3 offline check (0 game runs, headless fview): the offscreen maps take turns under LOW (docs\\renderer\\MAP_TURNS.md).

  py -I tools\\g3\\g3_check.py [--root DIR] [--part unit|wiring|replay] [--capture bryan|match] [--sheets DIR] [--frames 40,100,101,160]

unit    tools\\g3\\g3_clock_test.cpp against the real map_turns.h (the drawn-frame clock: step 1; the turn table), and against a planted copy (-DG3_PLANT_BIND_CLOCK: the clock
        counting binds, what binder.h did) which must FAIL.
wiring  each of the four switches is in the rdr_sw table, defined as a cvar in reader.h, read EVERY frame in live.cpp (BoolSeen) and gated on the LOW tier, and the planner's plan
        reaches the binder's op loop, the old half rate's three sites and the copy-alias exclusion; the binder's clock is the DrawnClock. Each link is removed in a copy: must FAIL.
replay  (every arm runs with --shadow-half, the LOW tier's half rate; reviewed F3) fview on a capture (Bryan entrance / the match): A = the game every frame (--shadows-ours), then one arm per map (wrestler chain, rim, props, pre-light) and all four
        (--turns MASK). Bars: validation 0; stale reads 0 (the cross-frame ledger: no reader sees an image older than one drawn frame); sweat skipped 0; passes / draws left out
        per frame at least the claim; the steps of the frame fall; a turns-off arm is hash-equal to A. A sheet per map (A | turns | difference x8). Planted twins (each must be
        caught by the ledger): every map stuck in its skip phase (a map skipped on the frame its reader needs it), and the sweat map put in a turn."""
import argparse
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
CAPTURES = {
    'bryan': 'out/s101/runs/20261112_122700_s100_owner_bryan_entrance/case_20261112_122719_969_0_pc_run_exit.zip',
    'match': 'out/s101/runs/20261112_131008_s100_owner_match/case_20261112_131027_265_0_pc_run_exit.zip',
}
# the claims, in left-out passes / draws a planned frame (a conservative bar: about 60 % of what Bryan / the match measured). The pre-light turn is not exposed (review F6: until G4's lazy re-bake exists).
MAPS = [  # (name, mask, report group, bryan passes, bryan draws, match passes, match draws)
    ('wrestler', 1, 'wrestler chain', 1.7, 18.0, 1.7, 38.0),
    ('rim', 2, 'rim map', 1.1, 11.0, 1.1, 22.0),
    ('props', 4, 'props / circles / plane', 1.4, 13.0, 1.4, 22.0),
]
fails = 0


def line(ok, text):
    global fails
    print(('PASS  ' if ok else 'FAIL  ') + text, flush=True)
    fails += 0 if ok else 1


sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'split'))
from read_unit import read_path  # a split file is read with its parts expanded (tools/split/read_unit.py)


def read(root, rel):
    return read_path(os.path.join(root, rel), crlf=True)


# ---------------------------------------------------------------------------------------------------------------- unit
def build_clock_test(root, work, tag, extra):
    d = os.path.join(work, tag)
    os.makedirs(d, exist_ok=True)
    exe = os.path.join(d, 'test.exe')
    src = os.path.join(root, 'tools', 'g3', 'g3_clock_test.cpp')
    cmd = [CLANG, '-std=c++20', '-O1', '-Wall', '-I' + os.path.join(root, 'src', 'renderer_adapter'), '-I' + os.path.join(root, 'src', 'renderer'), '-I' + os.path.join(root, 'src')] + extra + [src, '-o', exe, '-fuse-ld=lld']
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    if p.returncode:
        print(p.stdout + p.stderr)
        return None
    return exe


def part_unit(root):
    work = os.path.join(root, 'out', 'g3', 'unit')
    exe = build_clock_test(root, work, 'real', [])
    if not exe:
        line(False, 'the clock test does not build against the real header')
        return
    p = subprocess.run([exe], capture_output=True, text=True, timeout=60)
    line(p.returncode == 0 and 'all passed' in p.stdout, 'clock + turn table test passes on the real header: ' + p.stdout.strip().splitlines()[-1] if p.stdout.strip() else 'no output')
    exe2 = build_clock_test(root, work, 'plant_bind_clock', ['-DG3_PLANT_BIND_CLOCK'])
    if not exe2:
        line(False, 'the planted test does not build')
        return
    p2 = subprocess.run([exe2], capture_output=True, text=True, timeout=60)
    line(p2.returncode != 0 and 'FAIL' in p2.stdout, 'planted twin (the clock counting binds: lasting-only binds make the parity irregular) FAILS the test (%d failed lines)' % p2.stdout.count('FAIL'))
    exe3 = build_clock_test(root, work, 'plant_old_clock', ['-DG3_PLANT_OLD_CLOCK'])
    p3 = subprocess.run([exe3], capture_output=True, text=True, timeout=60) if exe3 else None
    line(p3 is not None and p3.returncode != 0 and 'FAIL' in p3.stdout, 'planted twin (the first G3 clock, no guest frame: a part whose rest is skipped freezes the count; review F1) FAILS the test (%d failed lines)' % (p3.stdout.count('FAIL') if p3 else -1))


# -------------------------------------------------------------------------------------------------------------- wiring
LINKS = [
    ('the four switches are in the rdr_sw table, PerFrame', 'src/renderer_adapter/rdr_switches.h',
     r'X\(TurnWrestler, "wwe2k14_rdr_turn_wrestler", PerFrame, 0\)[\s\S]*X\(TurnRim, "wwe2k14_rdr_turn_rim", PerFrame, 0\)[\s\S]*X\(TurnProps, "wwe2k14_rdr_turn_props", PerFrame, 0\)[\s\S]*X\(HalfRateDrawnClock, "wwe2k14_rdr_half_rate_drawn_clock", PerFrame, 0\)'),
    ('wrestler cvar defined, default off', 'src/renderer_adapter/reader.h', r'REXCVAR_DEFINE_BOOL\(wwe2k14_rdr_turn_wrestler, false'),
    ('rim cvar defined, default off', 'src/renderer_adapter/reader.h', r'REXCVAR_DEFINE_BOOL\(wwe2k14_rdr_turn_rim, false'),
    ('props cvar defined, default off', 'src/renderer_adapter/reader.h', r'REXCVAR_DEFINE_BOOL\(wwe2k14_rdr_turn_props, false'),
    ('step 1 cvar defined, default off', 'src/renderer_adapter/reader.h', r'REXCVAR_DEFINE_BOOL\(wwe2k14_rdr_half_rate_drawn_clock, false'),
    ('the LOW tier reaches the sink', 'src/renderer_adapter/reader.h', r'options\.low_tier\s*=\s*low;'),
    ('wrestler read every frame, gated on LOW', 'src/renderer_adapter/live.cpp', r'g_on\[mt::kWrestler\]\.store\(options\.low_tier && rdr_sw::BoolSeen\(rdr_sw::kTurnWrestler'),
    ('rim read every frame, gated on LOW', 'src/renderer_adapter/live.cpp', r'g_on\[mt::kRim\]\.store\(options\.low_tier && rdr_sw::BoolSeen\(rdr_sw::kTurnRim'),
    ('props read every frame, gated on LOW', 'src/renderer_adapter/live.cpp', r'g_on\[mt::kProps\]\.store\(options\.low_tier && rdr_sw::BoolSeen\(rdr_sw::kTurnProps'),
    ('step 1 switch read every frame', 'src/renderer_adapter/live.cpp', r'g_half_rate_drawn_clock\.store\(rdr_sw::BoolSeen\(rdr_sw::kHalfRateDrawnClock'),
    ('the pre-light turn is NOT exposed (review F6)', 'src/renderer_adapter/live.cpp', r'the pre-light turn is not exposed'),
    ('the binder counts DRAWN frames (DrawnClock), not binds', 'src/renderer_adapter/binder.h', r'drawn_frame_\s*=\s*drawn_clock_\.Bind\(persist_only_bind, stats\.isolate, frame_part_open, frame\.header\.guest_frame\)'),
    ('the old half rate keys on the drawn count only when the switch is on', 'src/renderer_adapter/binder.h', r'half_n = \(half_drawn \? drawn_frame_ : frame_number_\)'),
    ('the half rate is the old rule when the switch is off', 'src/renderer_adapter/binder.h', r'shadow_skip_frame_\s*=\s*g_shadow_half && \(!half_drawn \|\| !persist_only_bind\) && \(half_n & 1\) && half_n > 8'),
    ('the ledger sees the old half rate (review F2)', 'src/renderer_adapter/binder.h', r'turns_plan_, \[this\]\(uint32_t image\) \{ return OldHalfSkips\(image\); \}'),
    ('copy_alias books a dropped copy as kExcluded (G8: the one mechanism; review F7)', 'src/renderer_adapter/copy_alias.h', r'facts\.op_excluded && facts\.op_excluded\(p\)\) \{[^\n]*\n\s*ev_\.push_back\(\{c\.dest\.image, p, kExcluded, 0\}\);'),
    ('copy_alias plans no elide of a copy out of a half-rate / turn shadow map (review F7)', 'src/renderer_adapter/binder.h', r'\(g_shadow_half && shadow_sets_\.targets\.count\(id\)\) \|\| turns_planner_\.Tracks\(id\)'),
    ('owned_ is filled on every bind (review F2)', 'src/renderer_adapter/map_turns.h', r'Learn\(f, role_of\);\s*for \(const Span& s : spans_\)[^\n]*\n\s*if \(\(s\.group == kWrestler'),
    ('a span holding a D3 marker is never cut (review F5)', 'src/renderer_adapter/map_turns.h', r'if \(s\.marker && g_fault != Fault::CutMarkers\) \{ \+\+census\.marker_kept; continue; \}'),
    ('a span holding a Present is never cut (review F10)', 'src/renderer_adapter/map_turns.h', r'if \(s\.present\) \{ \+\+census\.present_kept; continue; \}'),
    ('the read age comes from the last kept write (review F4)', 'src/renderer_adapter/map_turns.h', r'const uint64_t age = never \? ~0ull : drawn - w->second;'),
    ('the key rule needs the shadow family (review F8)', 'src/renderer_adapter/map_turns.h', r'learned == kNoGroup && !s\.family_only'),
    ('the follower rule needs a role or a size (review F9)', 'src/renderer_adapter/map_turns.h', r'px_of\(s\.key\) <= chain_px\[g\]'),
    ('the planner runs every bind', 'src/renderer_adapter/binder.h', r'TurnsBegin\(frame, stats\);'),
    ('the op loop drops the plan', 'src/renderer_adapter/binder.h', r'\(turns_on_ && turns_plan_\.drop\[op_index\]\) \|\| \(plane_blank_on_'),
    ('SetTargets of the old half rate leaves the turn-owned images', 'src/renderer_adapter/binder.h', r'skipping_target_\s*=\s*OldHalfSkips\(current_target_\)'),
    ('Copy of the old half rate leaves the turn-owned images', 'src/renderer_adapter/binder.h', r'if \(OldHalfSkips\(copy\.source\.image\)\) (?:break|return);'),
    ('the copy-alias planner knows the dropped copies', 'src/renderer_adapter/binder.h', r'if \(turns_on_ && turns_plan_\.drop\[p\]\) return true;'),
    ('G4b sees the turns plan (ctx.others), after TurnsBegin', 'src/renderer_adapter/binder.h', r'TurnsBegin\(frame, stats\);[\s\S]{0,1200}PlaneBlankBegin\(frame, stats\);'),
    ('G4b gets the turns drop set', 'src/renderer_adapter/binder.h', r'ctx\.others\.push_back\(turns_plan_\.drop\.data\(\)\)'),
    ('the planner honours G4\'s plan', 'src/renderer_adapter/binder.h', r'dead_on_ \? &dead_plan_\.drop : nullptr'),
    ('the ledger is logged', 'src/renderer_adapter/binder.h', r'\[bind\]\[turns\] STALE READ'),
]


def part_wiring(root):
    texts = {}
    bad = 0
    for what, rel, rx in LINKS:
        texts.setdefault(rel, read(root, rel))
        ok = re.search(rx, texts[rel]) is not None
        bad += 0 if ok else 1
        line(ok, 'wiring: ' + what)
    # twins: remove each link from a copy; the check must then fail
    caught = 0
    for what, rel, rx in LINKS:
        m = re.search(rx, texts[rel])
        if not m:
            continue
        cut = texts[rel][:m.start()] + texts[rel][m.end():]
        if re.search(rx, cut) is None:
            caught += 1
    line(caught == len(LINKS) - bad, 'wiring twins: removing any one of the %d links is caught (%d of %d)' % (len(LINKS), caught, len(LINKS) - bad))
    # no hash-keyed identification in the planner (lane 86's rule: by origin, never a shader hash)
    t = read(root, 'src/renderer_adapter/map_turns.h')
    line(not re.search(r'ps_hash|vs_hash|vs_patched_hash', t), 'map_turns.h identifies draws by origin only (no shader hash)')
    line('kFeedback' not in t and 'FeedbackMap' in t, 'the sweat (feedback) maps are named only to be kept out of every turn')


# -------------------------------------------------------------------------------------------------------------- replay
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


PRE_G3 = '963237ed'   # s102-arc-pt2 as it was before G3 merged: the step-1-off arm's reference build (review F3)
COMMON = ['--shadows-ours', '--shadow-half']   # review F3: every arm (A included) runs the LOW half rate


def pre_g3_exe(root, main_co):
    """fview.exe built from the pre-G3 tree (git archive of PRE_G3, in <main>\\.claude\\worktrees\\g3_base_<rev>: tools\\fview\\build.ps1 finds ImGui from that depth). Cached."""
    base = os.path.join(main_co, '.claude', 'worktrees', 'g3_base_' + PRE_G3)
    exe = os.path.join(base, 'out', 'renderer', 'dev', 'fview.exe')
    if os.path.isfile(exe):
        return exe
    os.makedirs(base, exist_ok=True)
    tar = os.path.join(base, 'src.tar')
    p = subprocess.run(['git', '-C', root, 'archive', '-o', tar, PRE_G3, 'src', 'tools'], capture_output=True, text=True, timeout=600)
    if p.returncode:
        print(p.stdout + p.stderr)
        return None
    subprocess.run(['tar', '-xf', tar, '-C', base], capture_output=True, timeout=600)
    os.remove(tar)
    p = subprocess.run(['pwsh', '-NoProfile', '-File', os.path.join(base, 'tools', 'fview', 'build.ps1')], capture_output=True, text=True, timeout=2400)
    if p.returncode or not os.path.isfile(exe):
        print((p.stdout + p.stderr)[-2500:])
        return None
    return exe


class Arm:
    def __init__(self, exe, zip_path, game, work, name, extra, dump=None, frames=1000, base_flags=None):
        hashes = os.path.join(work, name + '.csv')
        if os.path.exists(hashes):
            os.remove(hashes)
        cmd = [exe, '--case', zip_path, '--replay', '--frames', str(frames), '--validation', '1', '--crowd-ours', '--crowd-cards', '--game', game, '--hash-frames', hashes] + (COMMON if base_flags is None else base_flags) + extra
        if dump:
            cmd += ['--dump-frames', ','.join(str(f) for f in dump[0]), '--dump-dir', dump[1]]
        for _ in range(12):
            p = subprocess.run(cmd, capture_output=True, text=True, timeout=1500, errors='replace')
            self.text = (p.stdout or '') + (p.stderr or '')
            if 'Not enough graphics memory for the frame ring' not in self.text:
                break
            time.sleep(20)
        self.code = p.returncode
        self.hashes = read_hashes(hashes) if os.path.isfile(hashes) else {}
        v = re.findall(r'validation errors (\d+)', self.text)
        self.valid = int(v[-1]) if v else -1
        m = re.search(r'STALE READS (\d+), sweat skipped (\d+)', self.text)
        self.stale, self.sweat = (int(m.group(1)), int(m.group(2))) if m else (None, None)
        m = re.search(r'planned (\d+) unplanned', self.text)
        self.planned = int(m.group(1)) if m else 0
        m = re.search(r'frames planned \d+ \(skipped \d+\), violations (\d+)', self.text)
        self.viol = int(m.group(1)) if m else None
        m = re.search(r'capture: (\d+) frames, (\d+) draws, (\d+) bound', self.text)   # fview's own tally: draws bound = draws recorded (the ones left out are not)
        self.bound = int(m.group(3)) / max(1, int(m.group(1))) if m else 0.0

    def group(self, name):
        """-> a report group line's numbers, or None"""
        m = re.search(r'\[rdr\]\[turns\]\s+' + re.escape(name) + r'\s+spans seen (\d+), left out (\d+) \(([\d.]+) passes, ([\d.]+) draws, ([\d.]+) copies a planned frame; kept by a guard (\d+)\), reads age 0 (\d+) age 1 (\d+) STALE (\d+)', self.text)
        if not m:
            return None
        return dict(seen=int(m.group(1)), out=int(m.group(2)), passes=float(m.group(3)), draws=float(m.group(4)), copies=float(m.group(5)), guard=int(m.group(6)), age0=int(m.group(7)), age1=int(m.group(8)), stale=int(m.group(9)))

    def differing(self, other):
        return sum(1 for k in self.hashes if k != 'format' and self.hashes.get(k) != other.hashes.get(k))


def sheet(dirs, frames, labels, out_png, title):
    from PIL import Image, ImageChops, ImageDraw
    rows = []
    frames = [f for f in frames if all(os.path.exists(os.path.join(d, 'frame_%05d.png' % f)) for d in dirs)]
    for f in frames:
        a, b = [Image.open(os.path.join(d, 'frame_%05d.png' % f)).convert('RGB') for d in dirs]
        diff = ImageChops.difference(a, b).point(lambda v: min(255, v * 8))
        rows.append((a, b, diff))
    if not rows:
        return False
    w, h = rows[0][0].size
    s = 0.5
    tw, th = int(w * s), int(h * s)
    pad = 26
    img = Image.new('RGB', (tw * 3 + 8, (th + pad) * len(rows) + pad), (24, 24, 24))
    d = ImageDraw.Draw(img)
    d.text((6, 6), title, fill=(255, 255, 255))
    for i, (a, b, diff) in enumerate(rows):
        y = pad + i * (th + pad)
        for j, (pic, lab) in enumerate(zip((a, b, diff), (labels[0], labels[1], 'difference x8'))):
            img.paste(pic.resize((tw, th)), (j * (tw + 4), y + pad - 12))
            d.text((j * (tw + 4) + 4, y), '%s  frame %d' % (lab, frames[i]), fill=(255, 220, 120))
    img.save(out_png)
    return True


def mean_abs_diff(dir_a, dir_b, frames):
    """the MEAN over the sheet frames of the mean |difference| per channel (review F10: it was the max)"""
    from PIL import Image, ImageChops, ImageStat
    vals = []
    for f in frames:
        pa, pb = os.path.join(dir_a, 'frame_%05d.png' % f), os.path.join(dir_b, 'frame_%05d.png' % f)
        if os.path.exists(pa) and os.path.exists(pb):
            diff = ImageChops.difference(Image.open(pa).convert('RGB'), Image.open(pb).convert('RGB'))
            vals.append(sum(ImageStat.Stat(diff).mean) / 3.0)
    return sum(vals) / len(vals) if vals else None


def part_replay(root, capture, sheets, frames):
    main_co = main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    z = os.path.join(main_co, CAPTURES[capture])
    if not os.path.exists(z):
        line(False, 'input missing: ' + z)
        return
    sys.path.insert(0, os.path.join(root, 'tools', 'fview'))
    import fview_exe as fe
    exe = fe.find_or_exit(root=root, kind='dev')
    work = os.path.join(root, 'out', 'g3', capture)
    os.makedirs(work, exist_ok=True)
    if sheets:
        os.makedirs(sheets, exist_ok=True)

    def dump(tag):
        d = os.path.join(work, 'dump_' + tag)
        os.makedirs(d, exist_ok=True)
        return (frames, d)

    A = Arm(exe, z, game, work, 'A', [], dump('A'))
    line(A.valid == 0 and len(A.hashes) > 20, '%s: A (LOW today: --shadow-half, the old per-bind count): %d frames, validation %d, draws recorded a frame %.1f' % (capture, len(A.hashes) - 1, A.valid, A.bound))
    # step 1 OFF = the old count, exactly as before: the PRE-G3 exe against this one, both --shadow-half, turns 0, hash-equal on every frame (review F3); plant: the clock moved one frame must differ
    base = pre_g3_exe(root, main_co)
    if base:
        P = Arm(base, z, game, work, 'pre_g3', [], base_flags=['--shadows-ours', '--shadow-half'])
        line(P.valid == 0 and len(P.hashes) == len(A.hashes) and P.differing(A) == 0, '%s: step 1 off (the default) = the pre-G3 exe (%s): %d of %d frames differ' % (capture, PRE_G3, P.differing(A), len(A.hashes) - 1))
        O = Arm(exe, z, game, work, 'offset1', ['--half-clock-offset', '1'])
        line(O.differing(P) > 0, '%s: planted twin (the half rate clock moved one frame) differs from the pre-G3 exe on %d frames: the comparison can fail' % (capture, O.differing(P)))
    else:
        line(False, 'the pre-G3 fview.exe could not be built (git archive %s)' % PRE_G3)
    off = Arm(exe, z, game, work, 'off', ['--turns', '0'])
    line(off.differing(A) == 0 and len(off.hashes) == len(A.hashes), '%s: turns off = no change: %d of %d frames differ from A' % (capture, off.differing(A), len(A.hashes) - 1))
    C = Arm(exe, z, game, work, 'clock_on', ['--half-drawn-clock', 'on'])
    line(C.valid == 0 and C.differing(A) == 0, '%s: the drawn clock on a replay with no skipped frames = the per-bind count: %d of %d frames differ' % (capture, C.differing(A), len(A.hashes) - 1))
    for name, mask, group, *claims in MAPS + [('all', 15, None, None, None, None, None)]:
        B = Arm(exe, z, game, work, name, ['--turns', str(mask)], dump(name))
        ok = B.valid == 0 and B.stale == 0 and B.sweat == 0 and len(B.hashes) == len(A.hashes) and B.planned > 20
        line(ok, '%s: %s turn: validation %d, STALE READS %s, sweat skipped %s, %d frames planned' % (capture, name, B.valid, B.stale, B.sweat, B.planned))
        if group:
            g = B.group(group)
            passes, draws = claims[0:2] if capture == 'bryan' else claims[2:4]
            line(g is not None and g['passes'] >= passes and g['draws'] >= draws and g['stale'] == 0,
                 '%s: %s: left out %.2f passes / %.1f draws / %.1f copies a planned frame (claim >= %.1f / %.1f); reads age 0 %d, age 1 %d, stale %d' %
                 (capture, name, g['passes'] if g else -1, g['draws'] if g else -1, g['copies'] if g else -1, passes, draws, g['age0'] if g else -1, g['age1'] if g else -1, g['stale'] if g else -1))
        d = B.differing(A)
        mad = mean_abs_diff(dump('A')[1], dump(name)[1], frames)
        line(mad is not None and mad < 4.0, '%s: %s: %d of %d frames differ from A; mean over the sheet frames of the mean |difference| %s (bar < 4/255: a gross break is a hole, a one-frame-old map is a few)' %
             (capture, name, d, len(A.hashes) - 1, 'n/a' if mad is None else '%.3f' % mad))
        if sheets:
            png = os.path.join(sheets, 'g3_%s_%s.png' % (name, capture))
            ok = sheet([dump('A')[1], dump(name)[1]], frames, ['LOW today (half rate)', 'turns: ' + name], png, 'G3 %s: %s capture, shadows ours, --shadow-half; A = the half rate today, B = --turns %d' % (name, capture, mask))
            line(ok, 'sheet: ' + png)
    # the planted split-frame arm (review F2): every 50th frame is bound as an open part; the old half rate must not leave out an image a turn owns, the ledger sees the old half rate
    S = Arm(exe, z, game, work, 'part50', ['--turns', '4', '--part-every', '50'])
    line(S.valid == 0 and S.stale == 0 and S.planned > 20, '%s: split frames planted every 50th frame (--part-every 50, --turns 4): STALE READS %s (it was 4 before F2: owned_ filled only on planned binds)' % (capture, S.stale))
    # planted twins: each must be caught by the ledger
    for name, mask, fault in (('wrestler', 1, 'stuck_wrestler'), ('rim', 2, 'stuck_rim'), ('props', 4, 'stuck_props'), ('sweat', 0, 'sweat_in_turn'), ('no guard', 1, 'no_guard')):
        T = Arm(exe, z, game, work, 'twin_' + fault, ['--turns', str(mask), '--turns-fault', fault], frames=80)
        extra_ok = (T.sweat and T.sweat > 0) if fault == 'sweat_in_turn' else True
        line(T.code == 0 and T.stale is not None and T.stale > 0 and extra_ok,
             '%s: planted twin %s is caught: STALE READS %s%s (the clean arm: 0)' % (capture, fault, T.stale, ', sweat skipped %s' % T.sweat if fault == 'sweat_in_turn' else ''))
    # review F7: copy_alias must not book a copy the props turn leaves out as a write (stream maps: no --shadows-ours): the pictures equal the arm without copy_alias, violations 0
    X0 = Arm(exe, z, game, work, 'alias_off', ['--turns', '4'], base_flags=['--shadow-half'])
    X1 = Arm(exe, z, game, work, 'alias_on', ['--turns', '4', '--copy-alias', '2', '--copy-alias-classes', '2'], base_flags=['--shadow-half'])
    line(X0.valid == 0 and X1.valid == 0 and X1.viol == 0 and X1.stale == 0 and X1.differing(X0) == 0 and len(X1.hashes) == len(X0.hashes) > 20,
         '%s: the props turn with copy_alias mode 2 class 2, stream maps: %d of %d frames differ from the same run without copy_alias, violations %s, stale %s' % (capture, X1.differing(X0), len(X0.hashes) - 1, X1.viol, X1.stale))
    G = Arm(exe, z, game, work, 'bad_phase', ['--turns', '1', '--turns-fault', 'bad_phase'])
    gw = G.group('wrestler chain')
    line(G.stale == 0 and gw is not None and gw['guard'] > 0, '%s: the age guard works: a bad phase (2 of 3 frames) WITH the guard keeps %s skips and gives STALE READS %s; without the guard (no_guard) it is caught' % (capture, gw and gw['guard'], G.stale))
    if capture == 'bryan':
        part_planeblank(exe, z, game, work)
        part_d3(root, exe, z, game, work)
        part_rplay(root, main_co)


def part_planeblank(exe, z, game, work):
    """G4b overlap: the props turn cuts the plane chain on alternate frames; G4b (--skip-blank-plane) must see that drop set (ctx.others) and decline, not take the turn-cut frame as its first run;
    its planted fault ignore_overlap must show the difference (it skips the chain on every frame, 236 of 237)"""
    res = {}
    for tag, extra in (('fix', []), ('ignore_overlap', ['--skip-blank-plane-fault', 'ignore_overlap'])):
        A = Arm(exe, z, game, work, 'pb_' + tag, ['--turns', '4', '--skip-blank-plane'] + extra)
        m = re.search(r'\[rdr\]\[plane_blank\] frames (\d+) .*?chains (\d+): .*?declined \(overlap (\d+), taken (\d+)\).*?first run (\d+), skipped (\d+)', A.text)
        res[tag] = (A, tuple(int(x) for x in m.groups()) if m else None)
    (fa, f), (ia, i) = res['fix'], res['ignore_overlap']
    line(f is not None and i is not None and fa.valid == 0 and fa.stale == 0 and f[2] > 0 and f[5] < f[1] // 2 and i[2] == 0 and i[5] > f[5],
         'bryan: G4b with the props turn on: declined_overlap %s, first run %s, skipped %s of %s chains (the turn-cut frames are not its first run); planted ignore_overlap: declined 0, skipped %s' %
         (f and f[2], f and f[4], f and f[5], f and f[1], i and i[5]))


def d3_arm(exe, z, game, work, tag, extra, ledger, ledger_out=False):
    d = os.path.join(work, 'd3_' + tag)
    cmd = [exe, '--case', z, '--game', game, '--frames', '237', '--thin-run', d, '--d3', 'casters', '--d3-mode', 'stream', ('--d3-ledger-out' if ledger_out else '--d3-ledger-in'), ledger] + extra
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=1500, errors='replace')
    text = (p.stdout or '') + (p.stderr or '')
    m = re.search(r'\[rdr\]\[d3\]\[casters\] mode \w+: markers (\d+), .*?unconsumed (\d+), gate resets (\d+)', text)
    return p.returncode, (int(m.group(1)), int(m.group(2)), int(m.group(3))) if m else None


def part_d3(root, exe, z, game, work):
    """review F5: a span holding a D3 marker is never cut: --d3 casters + the wrestler turn gives 0 unconsumed packets; the planted fault (the guard off) is caught"""
    ledger = os.path.join(work, 'd3_ledger.txt')
    rc, base = d3_arm(exe, z, game, work, 'p1', [], ledger, ledger_out=True)
    rc2, b = d3_arm(exe, z, game, work, 'p2', [], ledger)
    rc3, t = d3_arm(exe, z, game, work, 'p2turns', COMMON + ['--turns', '1'], ledger)
    rc4, c = d3_arm(exe, z, game, work, 'p2cut', COMMON + ['--turns', '1', '--turns-fault', 'cut_markers'], ledger)
    line(b is not None and t is not None and rc2 == 0 and rc3 == 0 and b[0] > 0 and t[0] == b[0] and t[1] == 0 and t[2] == 0,
         'bryan: --d3 casters with the wrestler turn on: markers %s (no turns %s), unconsumed %s, gate resets %s (a marker span is never cut)' % (t and t[0], b and b[0], t and t[1], t and t[2]))
    line(c is not None and (c[1] > 0 or c[2] > 0), 'bryan: planted twin cut_markers (the marker guard off) is caught: unconsumed %s, gate resets %s' % (c and c[1], c and c[2]))


def part_rplay(root, main_co):
    """rplay (the other offline player): --turns and --skip-every 3 (lasting-only binds between drawn frames, as the Surface drops frames)"""
    exe = os.path.join(root, 'out', 'renderer', 'dev', 'rplay.exe')
    z = os.path.join(main_co, CAPTURES['bryan'])
    dest = os.path.join(root, 'out', 'g3', 'rplay_unpack')   # rplay reads a capture FOLDER only: the case zip is unpacked the way fview does it (tar), used once, deleted
    cap = os.path.join(dest, 'capture')
    if not os.path.isfile(exe):
        line(False, 'rplay.exe is not built: ' + exe + ' (tools\\renderer\\build.ps1)')
        return
    import shutil
    shutil.rmtree(dest, ignore_errors=True)
    os.makedirs(dest, exist_ok=True)
    tar = os.path.join(os.environ.get('SystemRoot', r'C:\Windows'), 'System32', 'tar.exe')
    subprocess.run([tar, '-xf', z, '-C', dest], capture_output=True, timeout=900)
    if not os.path.isfile(os.path.join(cap, 'images.bin')):
        line(False, 'could not unpack ' + z)
        return
    outp = os.path.join(root, 'out', 'g3', 'rplay.png')
    p = subprocess.run([exe, '--capture', cap, '--frames', '237', '--validation', '1', '--shadow-half', '--turns', '15', '--skip-every', '3', '--out', outp], capture_output=True, text=True, timeout=1500, errors='replace')
    text = (p.stdout or '') + (p.stderr or '')
    m = re.search(r'STALE READS (\d+)', text)
    fr = re.search(r'frames (\d+) planned (\d+)', text)   # (rplay's exit code is its validation error count)
    shutil.rmtree(dest, ignore_errors=True)
    line(p.returncode == 0 and m is not None and int(m.group(1)) == 0 and fr is not None and int(fr.group(2)) > 20,
         'rplay (Bryan) --shadow-half --turns 15 --skip-every 3: exit %d (= validation errors), STALE READS %s, frames %s planned %s' % (p.returncode, m and m.group(1), fr and fr.group(1), fr and fr.group(2)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--part', default='all')
    ap.add_argument('--capture', default='bryan')
    ap.add_argument('--sheets', default='')
    ap.add_argument('--frames', default='')
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    if args.part == 'rplay':
        part_rplay(root, main_checkout(root))
        print('G3: %s' % ('all passed' if not fails else '%d FAILED' % fails))
        return 1 if fails else 0
    if args.part == 'base':
        print(pre_g3_exe(root, main_checkout(root)))
        return 0
    if args.part in ('all', 'unit'):
        part_unit(root)
    if args.part in ('all', 'wiring'):
        part_wiring(root)
    if args.part in ('all', 'replay'):
        frames = [int(x) for x in (args.frames or ('40,100,101,160' if args.capture == 'bryan' else '40,100,101,300')).split(',')]
        sheets = args.sheets or os.path.join(root, 'out', 's102', 'sheets', 'g3')   # the worktree's own out (review F10); the main session copies what Jarrod scrolls back to
        part_replay(root, args.capture, sheets, frames)
        if os.path.isdir(sheets):
            import datetime
            until = (datetime.date.today() + datetime.timedelta(days=7)).isoformat()
            with open(os.path.join(sheets, 'KEEP'), 'w') as f:
                f.write('reason: lane G3 sheets (the LOW half rate today vs the maps taking turns, per map, Bryan entrance + match): the re-check Jarrod asked for of the rim map / light strip half rates on the new renderer\nuntil %s\n' % until)
    print('G3: %s' % ('all passed' if not fails else '%d FAILED' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
