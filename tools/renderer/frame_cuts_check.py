r"""Lane G11 (S102) offline proof: the frame buffer's two cuts (src\renderer_adapter\frame_cuts.h; docs\renderer\FRAME_CLASS.md), replayed by fview and rplay on captures. 0 game runs.
  py -I tools\renderer\frame_cuts_check.py [--no-timing] [--quick]
Arms (fview --replay, validation on, per-frame picture hashes; switches --frame-selfblit --frame-restore):
  unit        the rules on plain facts (frame_cuts_test.cpp): every rule declines on its own fault, the write versions, the stencil mark; six planted faults must each FAIL the test.
  per capture off / selfblit / restore / both: the picture hashes byte-equal to off, validation errors 0 AND exit code 0 (fview writes its csv before it exits 4 on errors),
              bound draws fall by exactly the draws taken (selfblit 1 a frame, restore 3 a frame on the entrance captures), the census line is read (frames taken are reported).
  bundle      the same under this check's BUNDLE (--shadows-ours --shadow-casters-ours --plane-shadow-ours --post-ours --small-passes-ours --rim-ours --rim-chain-ours --circles-ours
              --movie-ours --screens-ours): hashes equal to the bundle without the cuts, frames taken reported (0 taken = the lever does nothing for players).
  alias       --copy-alias 2 --copy-alias-classes 63 and --alias-fullsize 2: the cuts and G8's alias together (a restore's copies read an aliased texture).
  scale       --scale 1.5 and 0.75 (F3): hashes equal, the cuts still take every frame (every image is scaled alike).
  persist     rplay --persist-only-every 2: exactly the persist-only frames are counted unusable, the rest still take, the final picture is equal.
  sweep       fview --sweep with images 8 / 9 (colour and DEPTH of the frame buffer hashed each frame): off == restore; the restore_no_depth twin differs.
Planted twins (must be CAUGHT): --frame-cuts-fault blit_always (every sprite-shader draw left out: the pictures must differ), restore_swap (the sources swapped: a crash or validation errors), restore_other_source
(a LEGAL copy from another 1280x720 colour image: exit 0, validation 0, the pictures differ), restore_no_depth (the depth not copied: the sweep hashes differ).
Timing (rplay, the PC's GPU is a 4090: the frame class is 0.08 ms there, so this is a count check, not the Gen9 number): interleaved pairs off / both; the shader-written pixels and the frame class's pass
count come from the frame trace (rplay --frame-trace) and must fall as claimed.
Exit 1 on any failure or when an input cannot be read."""
import os
import re
import statistics
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
MAIN = os.environ.get('RDR_MAIN_CHECKOUT')
if not MAIN:
    try:
        common = subprocess.run(['git', '-C', ROOT, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        MAIN = os.path.dirname(common) if common else ROOT
    except Exception:
        MAIN = ROOT


def exe(name):
    p = os.path.join(ROOT, 'out', 'renderer', 'dev', name)
    return p if os.path.exists(p) else os.path.join(MAIN, 'out', 'renderer', 'dev', name)


FVIEW, RPLAY = exe('fview.exe'), exe('rplay.exe')
CAPS = {'gate_bryan': ('s101/lane92/case_20261112_122719_969_0_pc_run_exit/capture', 100, (1, 3)),   # (blit, restore) taken per frame; None = reported only
        'bryan86': ('lane86/bryan/capture', 91, (1, 3)),
        'brock': ('lane86/brock/capture', 120, None),
        'match': ('lane86/wm30/capture', 120, None)}   # the 30 Years of WrestleMania match (423 frames): no entrance, so no restore and no self blit: the cuts must change nothing there
BUNDLE_NO_SCREENS = ['--shadows-ours', '--shadow-casters-ours', '--plane-shadow-ours', '--post-ours', '--small-passes-ours', '--rim-ours', '--rim-chain-ours', '--circles-ours', '--movie-ours']   # (the match capture has no scene records: --screens-ours stops fview there)
BUNDLE = ['--shadows-ours', '--shadow-casters-ours', '--plane-shadow-ours', '--post-ours', '--small-passes-ours', '--rim-ours', '--rim-chain-ours', '--circles-ours', '--movie-ours', '--screens-ours']
OUT = os.path.join(ROOT, 'out', 's102', 'g11')
FAILS = []


def say(ok, text):
    print(('PASS ' if ok else 'FAIL ') + text, flush=True)
    if not ok:
        FAILS.append(text)


CUTS = re.compile(r'\[rdr\]\[frame_cuts\] frames (\d+) \(unusable (\d+), of them with unknown targets (\d+)\); self blit (on|off): seen (\d+), taken (\d+) \(([\d.]+) Mpx\), declined(.*?) restore (on|off): seen (\d+), taken (\d+) \(([\d.]+) Mpx\), declined(.*)')


def run(tag, cap, frames, extra, env=None, allow_fail=False):
    os.makedirs(OUT, exist_ok=True)
    csv = os.path.join(OUT, tag + '.csv')
    if os.path.exists(csv):
        os.remove(csv)
    cmd = [FVIEW, '--capture', os.path.join(MAIN, 'out', cap), '--replay', '--frames', str(frames), '--validation', '1', '--hash-frames', csv] + extra
    e = dict(os.environ)
    e.update(env or {})
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=1500, env=e)
    text = p.stdout + p.stderr
    errs = re.search(r'validation errors (\d+)', text)
    if not allow_fail and (p.returncode != 0 or not os.path.exists(csv) or not errs):
        print(text[-1500:])
        say(False, '%s: fview exit %d (the csv exists: %s, error count read: %s)' % (tag, p.returncode, os.path.exists(csv), bool(errs)))
        return None
    hashes = [l.strip() for l in open(csv) if l.strip() and not l.startswith('format')] if os.path.exists(csv) else []
    m = re.search(r'capture: (\d+) frames, (\d+) draws, (\d+) bound', text)
    bound = int(m.group(3)) if m else -1
    c = CUTS.search(text)
    rep = None
    if c:
        rep = dict(frames=int(c.group(1)), unusable=int(c.group(2)), unknown=int(c.group(3)), blit_seen=int(c.group(5)), blit_taken=int(c.group(6)), blit_mpx=float(c.group(7)), restore_seen=int(c.group(10)), restore_taken=int(c.group(11)),
                   restore_mpx=float(c.group(12)), blit_declined=c.group(8).strip(' ,'), restore_declined=c.group(13).strip(' ,'))
    return dict(hashes=hashes, bound=bound, rep=rep, errors=int(errs.group(1)) if errs else -1, rc=p.returncode)


def same(a, b, frames):
    return a is not None and b is not None and a['hashes'] == b['hashes'] and len(a['hashes']) == frames


def unit_test():
    clang = r'C:\Program Files\LLVM\bin\clang++.exe'
    os.makedirs(OUT, exist_ok=True)
    exe_path = os.path.join(OUT, 'frame_cuts_test.exe')
    b = subprocess.run([clang, '-std=c++20', '-Wall', '-Wextra', '-I', os.path.join(ROOT, 'src'), '-I', os.path.join(ROOT, 'src', 'renderer_adapter'),
                        os.path.join(ROOT, 'src', 'renderer_adapter', 'frame_cuts_test.cpp'), '-o', exe_path], capture_output=True, text=True)
    if b.returncode:
        print(b.stdout + b.stderr)
        say(False, 'frame_cuts_test.cpp did not compile')
        return
    u = subprocess.run([exe_path], capture_output=True, text=True)
    say(u.returncode == 0, 'unit test (the walk\'s op calls: draws / clears / copies / Foreign writes / unissued copies / unknown targets; the blit and the restore rule by rule, shaders, c28, scaling, clip-off, aliases): %s' %
        (u.stdout.strip().splitlines() or ['no output'])[-1])
    for plant in ('plant_no_version', 'plant_no_tint', 'plant_no_rect', 'plant_no_stencil', 'plant_op_writes_off', 'plant_note_unissued', 'plant_no_proj', 'plant_no_scaled', 'plant_no_clip_off',
                  'plant_no_unknown_targets', 'plant_blit_always', 'plant_restore_swap'):
        u = subprocess.run([exe_path, plant], capture_output=True, text=True)
        say(u.returncode == 1 and 'FAILED' in u.stdout, 'unit test twin %s: CAUGHT (%d failing checks)' % (plant, u.stdout.count('FAIL ')))


def arms(name, cap, frames, per_frame, quick):
    off = run(name + '_off', cap, frames, [])
    if off is None:
        return
    res = {}
    for tag, flags in (('selfblit', ['--frame-selfblit']), ('restore', ['--frame-restore']), ('both', ['--frame-selfblit', '--frame-restore'])):
        r = run('%s_%s' % (name, tag), cap, frames, flags)
        if r is None:
            continue
        res[tag] = r
        say(same(off, r, frames) and off['errors'] == 0 and r['errors'] == 0 and r['rc'] == 0, '%s %s: %d frames, picture hashes %s, validation errors %d / %d, exit %d' %
            (name, tag, frames, 'equal' if off['hashes'] == r['hashes'] else 'DIFFER', off['errors'], r['errors'], r['rc']))
        rep = r['rep']
        say(rep is not None, '%s %s: the census line is read: %s' % (name, tag, rep))
        if rep is None:
            continue
        taken = (rep['blit_taken'] if tag != 'restore' else 0) + (rep['restore_taken'] * 2 // 2 if tag != 'selfblit' else 0)
        say(off['bound'] - r['bound'] == taken, '%s %s: bound draws %d -> %d (falls by the %d draws taken: blit %d + restore %d)' %
            (name, tag, off['bound'], r['bound'], taken, rep['blit_taken'] if tag != 'restore' else 0, rep['restore_taken'] if tag != 'selfblit' else 0))
    if per_frame and 'both' in res and res['both']['rep']:
        rep = res['both']['rep']
        say(rep['blit_taken'] == per_frame[0] * frames and rep['restore_taken'] == per_frame[1] * frames, '%s: the entrance takes %d self blit + %d restores a frame in every frame (taken %d / %d of %d frames)' %
            (name, per_frame[0], per_frame[1], rep['blit_taken'], rep['restore_taken'], frames))
    elif 'both' in res and res['both']['rep']:
        rep = res['both']['rep']
        print('  %s: frames taken (reported, not asserted): self blit %d of %d seen, restore %d of %d seen; declined: blit [%s] restore [%s]' %
              (name, rep['blit_taken'], rep['blit_seen'], rep['restore_taken'], rep['restore_seen'], rep['blit_declined'], rep['restore_declined']))
    return off, res


def timing(cap, frames, pairs):
    """rplay, interleaved pairs: the bracket's mean GPU ms and the frame class's ms (stamps) with the cuts off / on, the passes the frame class holds a frame."""
    rows = {'off': [], 'on': []}
    for i in range(pairs):
        for tag, flags in (('off', []), ('on', ['--frame-selfblit', '--frame-restore'])) if i % 2 == 0 else (('on', ['--frame-selfblit', '--frame-restore']), ('off', [])):
            p = subprocess.run([RPLAY, '--capture', os.path.join(MAIN, 'out', cap), '--frames', str(frames), '--gpu-stamps', '1', '--gpu-log', os.path.join(OUT, 'gpu_%s.csv' % tag)] + flags,
                               capture_output=True, text=True, timeout=900)
            text = p.stdout + p.stderr
            m = re.search(r'summary: frames (\d+), gpu ms median ([\d.]+) / mean ([\d.]+)', text)
            r = re.search(r'summary by role[^:]*: .*? frame ([\d.]+), copies ([\d.]+).*?passes a frame by class: .*?frame ([\d.]+),', text)
            if p.returncode != 0 or not m or not r:
                say(False, 'rplay timing %s: exit %d, summary read %s %s' % (tag, p.returncode, bool(m), bool(r)))
                return
            rows[tag].append((float(m.group(3)), float(r.group(1)), float(r.group(2)), float(r.group(3))))
    for tag in ('off', 'on'):
        print('  rplay %s: mean GPU ms %s; frame class ms %s; copies ms %s; frame passes a frame %s' % (tag, ['%.3f' % x[0] for x in rows[tag]], ['%.3f' % x[1] for x in rows[tag]],
                                                                                                   ['%.3f' % x[2] for x in rows[tag]], ['%.1f' % x[3] for x in rows[tag]]))
    med = lambda tag, k: statistics.median(x[k] for x in rows[tag])
    print('  medians (4090, %d pairs): bracket %.3f -> %.3f ms; frame class %.3f -> %.3f ms; copies %.3f -> %.3f ms; frame passes a frame %.1f -> %.1f. The 4090 spread between runs of one arm is %.3f ms: the bracket is noise-limited, the pass count is exact.' %
          (pairs, med('off', 0), med('on', 0), med('off', 1), med('on', 1), med('off', 2), med('on', 2), med('off', 3), med('on', 3), max(x[0] for x in rows['off']) - min(x[0] for x in rows['off'])))
    say(med('on', 3) <= med('off', 3) - 1.5, 'the frame class holds %.1f passes a frame with the cuts, %.1f without (at least 1.5 fewer on the entrance)' % (med('on', 3), med('off', 3)))


def trace_pixels(cap, frames):
    """The frame trace's shader-written pixels a frame (an occlusion query per draw), off and on."""
    got = {}
    for tag, flags in (('off', []), ('on', ['--frame-selfblit', '--frame-restore'])):
        tr = os.path.join(OUT, 'trace_%s.jsonl' % tag)
        p = subprocess.run([RPLAY, '--capture', os.path.join(MAIN, 'out', cap), '--frames', str(frames), '--frame-trace', tr] + flags, capture_output=True, text=True, timeout=900)
        if p.returncode != 0 or not os.path.exists(tr):
            say(False, 'rplay --frame-trace %s: exit %d' % (tag, p.returncode))
            return
        px, fr = 0, set()
        for line in open(tr, encoding='utf-8'):
            if '"t":"o"' in line:
                m = re.search(r'"f":(\d+).*"px":(\d+)', line)
                px += int(m.group(2))
                fr.add(int(m.group(1)))
        got[tag] = px / max(1, len(fr))
    mpx = lambda v: v / 921600.0
    print('  shader-written pixels a frame in the frame class: %.2f -> %.2f frame buffers (%.0f -> %.0f Kpx)' % (mpx(got['off']), mpx(got['on']), got['off'] / 1e3, got['on'] / 1e3))
    say(got['on'] <= got['off'] - 2.0 * 921600, 'the shader-written pixels fall by at least two frame buffers a frame (%.2f -> %.2f)' % (mpx(got['off']), mpx(got['on'])))


def png_sha(path):
    import hashlib
    return hashlib.sha256(open(path, 'rb').read()).hexdigest() if os.path.exists(path) else None


def persist_arm(cap, every=2, frames=40):
    """rplay --persist-only-every N binds every N-th frame as a skipped frame's lasting work (binder.persist_only_bind): the cuts must decline exactly those frames (counted unusable) and the
    final picture must not change."""
    res = {}
    for tag, flags in (('off', []), ('on', ['--frame-selfblit', '--frame-restore'])):
        png = os.path.join(OUT, 'persist_%s.png' % tag)
        if os.path.exists(png):
            os.remove(png)
        p = subprocess.run([RPLAY, '--capture', os.path.join(MAIN, 'out', cap), '--frames', str(frames), '--persist-only-every', str(every), '--out', png] + flags, capture_output=True, text=True, timeout=900)
        text = p.stdout + p.stderr
        res[tag] = (p.returncode, png_sha(png), CUTS.search(text))
    off, on = res['off'], res['on']
    say(off[0] == 0 and on[0] == 0 and off[1] is not None and off[1] == on[1], 'persist-only binds (rplay --persist-only-every %d, %d frames): final picture %s, exit %d / %d' % (every, frames, 'equal' if off[1] == on[1] else 'DIFFERS', off[0], on[0]))
    c = on[2]
    if not c:
        say(False, 'persist: no census line')
        return
    unusable, blit_taken, restore_taken = int(c.group(2)), int(c.group(6)), int(c.group(11))
    say(unusable == frames // every and blit_taken == frames - frames // every and restore_taken == 3 * (frames - frames // every),
        'persist: %d of %d frames counted unusable (every %d-th), the other frames still take: self blit %d, restore %d' % (unusable, frames, every, blit_taken, restore_taken))


def sweep(tag, cap, frames, flags):  # noqa
    """fview --sweep: per frame the picture hash and the hash of images 8 and 9 (the frame buffer's colour and DEPTH; the depth is read back as its raw 4-byte bits)."""
    csv = os.path.join(OUT, 'sweep_%s.csv' % tag)
    if os.path.exists(csv):
        os.remove(csv)
    p = subprocess.run([FVIEW, '--capture', os.path.join(MAIN, 'out', cap), '--replay', '--frames', str(frames), '--validation', '1', '--sweep', csv, '--sweep-images', '8,9'] + flags, capture_output=True, text=True, timeout=1500)
    errs = re.search(r'validation errors (\d+)', p.stdout + p.stderr)
    rows = [l.strip().split(',') for l in open(csv)][1:frames + 1] if os.path.exists(csv) else []   # (the sweep plays the whole capture: the first `frames` rows)
    return dict(rows=rows, errors=int(errs.group(1)) if errs else -1, rc=p.returncode)


def main():
    quick = '--quick' in sys.argv
    for path in (FVIEW, RPLAY):
        if not os.path.exists(path):
            print('FAIL no %s (tools\\renderer\\build.ps1, tools\\fview\\build.ps1)' % path)
            sys.exit(1)
    for name, (cap, frames, _) in CAPS.items():
        if not os.path.isdir(os.path.join(MAIN, 'out', cap)):
            print('FAIL cannot read capture', cap)
            sys.exit(1)
    unit_test()
    base = {}
    for name, (cap, frames, per_frame) in CAPS.items():
        if quick and name not in ('gate_bryan',):
            continue
        out = arms(name, cap, frames, per_frame, quick)
        if out:
            base[name] = out
    cap, frames, _ = CAPS['gate_bryan']
    both = ['--frame-selfblit', '--frame-restore']
    extra = [('bundle', BUNDLE, None, 'the default bundle (%s)' % ' '.join(BUNDLE)),
             ('alias63', ['--copy-alias', '2', '--copy-alias-classes', '63'], None, 'G8: --copy-alias 2 --copy-alias-classes 63'),
             ('full2', ['--alias-fullsize', '2'], None, 'G8: --alias-fullsize 2'),
             ('scale15', ['--scale', '1.5'], None, 'render scale 1.5 (F3: every image scaled)'),
             ('scale075', ['--scale', '0.75'], None, 'render scale 0.75')]
    if not quick:
        extra.append(('bundle_match', BUNDLE_NO_SCREENS, None, 'the default bundle (without --screens-ours) on the match'))
    for tag, flags, env, what in extra:
        c, f = (CAPS['match'][:2] if tag == 'bundle_match' else (cap, frames))
        if tag == 'bundle_match' and not os.path.isdir(os.path.join(MAIN, 'out', c)):
            say(False, 'cannot read capture ' + c)
            continue
        a = run('%s_%s_off' % ('x', tag), c, f, flags, env)
        b = run('%s_%s_on' % ('x', tag), c, f, flags + both, env)
        if a is None or b is None:
            continue
        rep = b['rep']
        say(same(a, b, f) and a['errors'] == 0 and b['errors'] == 0 and b['rc'] == 0, '%s: %d frames, hashes %s with the cuts on, validation %d / %d' % (what, f, 'equal' if a['hashes'] == b['hashes'] else 'DIFFER', a['errors'], b['errors']))
        say(rep is not None, '%s: frames taken reported: %s' % (tag, rep and ('self blit %d, restore %d (unusable frames %d)' % (rep['blit_taken'], rep['restore_taken'], rep['unusable']))))
        if tag in ('bundle', 'alias63', 'full2', 'scale15', 'scale075') and rep:
            say(rep['blit_taken'] == f and rep['restore_taken'] == 3 * f, '%s: the cuts still take their frames under it (%d self blit, %d restore of %d frames)' % (tag, rep['blit_taken'], rep['restore_taken'], f))
    # planted twins
    base_hashes = base.get('gate_bryan', (None, None))[0]
    if base_hashes is not None:
        t = run('gate_blit_always', cap, frames, ['--frame-selfblit', '--frame-cuts-fault', 'blit_always'], allow_fail=True)
        if t is not None:
            diff = sum(1 for i in range(min(frames, len(t['hashes']))) if t['hashes'][i] != base_hashes['hashes'][i])
            say(diff >= frames // 2, 'twin blit_always (every sprite-shader draw left out): CAUGHT, pictures differ on %d of %d frames' % (diff, frames))
        t = run('gate_restore_other', cap, frames, ['--frame-restore', '--frame-cuts-fault', 'restore_other_source'], allow_fail=True)
        if t is not None:
            diff = sum(1 for i in range(min(frames, len(t['hashes']))) if t['hashes'][i] != base_hashes['hashes'][i])
            say(t['rc'] == 0 and t['errors'] == 0 and diff >= frames // 2, 'twin restore_other_source (a LEGAL copy: another 1280x720 colour source): CAUGHT, exit %d, validation errors %d, pictures differ on %d of %d frames' % (t['rc'], t['errors'], diff, frames))
        t = run('gate_restore_swap', cap, frames, ['--frame-restore', '--frame-cuts-fault', 'restore_swap'], allow_fail=True)
        if t is not None:
            diff = sum(1 for i in range(min(frames, len(t['hashes']))) if t['hashes'][i] != base_hashes['hashes'][i])
            say(t['errors'] > 0 or t['rc'] != 0 or diff >= frames // 2, 'twin restore_swap (colour and depth sources swapped): CAUGHT, validation errors read %d, fview exit %d (a crash on the invalid copy counts), pictures differ on %d of %d frames' % (t['errors'], t['rc'], diff, frames))
    persist_arm(CAPS['bryan86'][0])
    c, f, _ = CAPS['bryan86']
    a = sweep('off', c, 40, [])
    b = sweep('restore', c, 40, ['--frame-restore'])
    n = sweep('nodepth', c, 40, ['--frame-restore', '--frame-cuts-fault', 'restore_no_depth'])
    say(a['errors'] == 0 and b['errors'] == 0 and a['rc'] == 0 and b['rc'] == 0 and len(a['rows']) == 40 and a['rows'] == b['rows'], 'sweep (picture, image 8 colour and image 9 DEPTH hashed each frame, 40 frames): off == restore on, validation %d / %d, exit %d / %d' % (a['errors'], b['errors'], a['rc'], b['rc']))
    if len(a['rows']) == 40 and len(n['rows']) == 40:
        diff = sum(1 for i in range(40) if a['rows'][i] != n['rows'][i])
        depth_distinct = len({r[3] for r in a['rows']})
        say(diff >= 20, 'twin restore_no_depth (the colour copied, the depth not): CAUGHT, the picture / colour / depth hashes differ on %d of 40 frames (the depth hash alone: this capture clears the frame buffer\'s depth at the end of every frame, %d distinct depth hashes in the off arm, so the pictures carry the proof)' % (diff, depth_distinct))
    else:
        say(False, 'sweep arms did not produce 40 rows')
    if '--no-timing' not in sys.argv:
        c, f, _ = CAPS['bryan86']
        trace_pixels(c, 60)
        timing(c, 90, 6 if not quick else 2)
    print('RESULT', 'FAIL' if FAILS else 'PASS')
    sys.exit(1 if FAILS else 0)


if __name__ == '__main__':
    main()
