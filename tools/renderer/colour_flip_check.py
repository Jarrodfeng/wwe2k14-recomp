r"""S102 lane R24-D part 1: offline proof that the two colour-pass levers can be flipped MID-RUN (the Surface's in-run A/B, docs\renderer\IN_RUN_AB.md), 0 game runs, 0 Surface runs.
  py -I tools\renderer\colour_flip_check.py [--root DIR] [--only bryan|brock]
Levers: G10 `wwe2k14_rdr_low_colour_cuts` (Int, mask bit 1) and C4 `wwe2k14_rdr_arena_overdraw` (Bool). Both are PerFrame rows of rdr_switches.h read through IntSeen / BoolSeen with the guest frame
(live.cpp) and carried on the frame; this check proves the RENDERER side (renderer.cpp RecordPass, binder.h, the pipelines, the texture-alpha table) holds nothing that a flip breaks:
  steady      the capture replayed with the lever at 0 and at 1 all the way (shots after chosen frames).
  flipped     the same capture with the lever flipped every 60 frames (rplay --arms on=..:1|off=..:0 --arm-period 60). Every shot of the flipped run must be BYTE-EQUAL to the steady run of the value
              that frame ran under (shots at the last frame of a phase and the FIRST frame after each flip: any state carried over a flip would show there).
  taken       the flipped run's census line shows the lever took some frames and not all (a flip that does nothing is not a proof).
  not vacuous the steady 0 and steady 1 pictures differ on at least one shot (else "equal" proves nothing). Planted twin: the flipped shots compared with the OPPOSITE value's steady run must
              show a difference (the comparison can fail).
  scan        C4 only: the texture-alpha scan (binder.h ScanImageAlpha) runs only while the switch is on. Replaying with `--arena-scan-follow` (the scan flag follows the arm, as live.cpp sets it) and the
              lever OFF in the first phase leaves textures that arrived then unproven: the on phases take FEWER draws (the finding that makes `wwe2k14_rdr_arena_overdraw_scan` necessary for a flip). Reported,
              not judged; with the hold on (the default arm parse) the flipped run is byte-equal to steady.
Counts and numbers only (hard rule 1); pictures go under out\ only."""
import argparse, concurrent.futures, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import colour_cuts_check as cc  # run_rplay, shot_diffs, BUNDLE, main_checkout, TAKEN_RE

FAILS = cc.FAILS
say = cc.say
PERIOD = 60
CAPTURES = {
    'bryan': ('s101/lane92/case_20261112_122719_969_0_pc_run_exit/capture', 237, [59, 60, 119, 120, 179, 180, 236]),
    'brock': ('s101/lane92/case_20261113_093550_855_0_pc_run_exit/capture', 375, [59, 60, 119, 120, 179, 180, 239, 240, 299, 300, 370]),
}
ARENA_RE = re.compile(r'\[rplay\]\[overdraw\] arena overdraw (\d+): texture-alpha proof refused (\d+) draws .*?admitted (\d+)')


def lever_at(frame):
    """1 = the lever ON for this frame in a flipped run whose first phase is ON."""
    return 1 if (frame // PERIOD) % 2 == 0 else 0


def equal_shots(flipped, steady_on, steady_off, shots, invert=False):
    """per shot: byte-equal to the steady run of the value the frame ran under? returns (n_equal, bad list)."""
    bad = []
    for f in shots:
        want = lever_at(f) ^ (1 if invert else 0)
        ref = steady_on if want else steady_off
        d = cc.shot_diffs(ref, flipped, [f])
        if d is None:
            bad.append((f, 'missing'))
        elif d[0]['nd'] != 0:
            bad.append((f, d[0]['nd']))
    return len(shots) - len(bad), bad


def run_one(root, name, outdir, game, main_co):
    sub, frames, shots = CAPTURES[name]
    capture = os.path.join(main_co, 'out', *sub.split('/'))
    if not os.path.isfile(os.path.join(capture, 'images.bin')):
        say(False, '%s: cannot read the capture %s' % (name, capture))
        return
    sh = ','.join(map(str, shots))

    def job(tag, extra):
        return ['--shots', sh, '--shot-dir', os.path.join(outdir, name + '_' + tag + '_shots')] + extra

    arms_g10 = '--arms'
    jobs = {
        'g_off': job('g_off', []),
        'g_on': job('g_on', ['--colour-cuts', '1']),
        'g_flip': job('g_flip', [arms_g10, 'on=3:1|off=3:0', '--arm-period', str(PERIOD), '--arm-skip', '0']),
        'a_off': job('a_off', []),
        'a_on': job('a_on', ['--arena-overdraw', '1']),
        'a_flip': job('a_flip', ['--arms', 'on=4:1|off=4:0', '--arm-period', str(PERIOD), '--arm-skip', '0']),
        # first phase OFF with the scan following the switch (live.cpp): the finding
        'a_follow': job('a_follow', ['--arms', 'off=4:0|on=4:1', '--arm-period', str(PERIOD), '--arm-skip', '0', '--arena-scan-follow']),
    }
    res = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as ex:
        futs = {k: ex.submit(cc.run_rplay, root, capture, game, name + '_' + k, outdir, v, 1500, frames) for k, v in jobs.items()}
        for k, f in futs.items():
            res[k] = f.result()
    for k, r in res.items():
        say(r['rc'] == 0, '%s %s: rplay exit code %d' % (name, k, r['rc']))
    if FAILS and any(r['rc'] != 0 for r in res.values()):
        return
    # G10
    n, bad = equal_shots(res['g_flip'], res['g_on'], res['g_off'], shots)
    say(not bad, '%s G10 low_colour_cuts flipped every %d frames: %d of %d shots byte-equal to the steady run of their phase%s' % (name, PERIOD, n, len(shots), '' if not bad else ' (differ: %s)' % bad))
    van = cc.shot_diffs(res['g_off'], res['g_on'], shots)
    nv = sum(1 for d in van if d['nd']) if van else 0
    say(nv > 0 or name != 'bryan', '%s G10 not vacuous (Bryan only: on Brock the levers take frames but the shots equal the unflipped pictures, byte for byte): steady 0 vs steady 1 differ on %d of %d shots (max diff %s)' % (name, nv, len(shots), max((d['mx'] for d in van), default=0) if van else '-'))
    n2, bad2 = equal_shots(res['g_flip'], res['g_on'], res['g_off'], shots, invert=True)
    say(len(bad2) > 0 or nv == 0, '%s G10 planted twin (flipped shots compared with the OPPOSITE phase value): %d of %d shots differ (must be > 0 when 0 vs 1 differ)' % (name, len(bad2), len(shots)))
    t, t1, t0 = res['g_flip'].get('taken'), res['g_on'].get('taken'), res['g_off'].get('taken') or 0   # (the census line is printed only while a lever is on: no line = 0)
    say(t is not None and t1 is not None and t0 == 0 and 0 < t < t1, '%s G10 taken: steady on %s colour passes with a prepass, steady off %s, flipped %s (must be between, and > 0)' % (name, t1, t0, t))
    # C4
    n, bad = equal_shots(res['a_flip'], res['a_on'], res['a_off'], shots)
    say(not bad, '%s C4 arena_overdraw flipped every %d frames (scan held): %d of %d shots byte-equal to the steady run of their phase%s' % (name, PERIOD, n, len(shots), '' if not bad else ' (differ: %s)' % bad))
    van = cc.shot_diffs(res['a_off'], res['a_on'], shots)
    nv = sum(1 for d in van if d['nd']) if van else 0
    say(nv > 0 or name != 'bryan', '%s C4 not vacuous (Bryan only): steady 0 vs steady 1 differ on %d of %d shots' % (name, nv, len(shots)))
    n2, bad2 = equal_shots(res['a_flip'], res['a_on'], res['a_off'], shots, invert=True)
    say(len(bad2) > 0 or nv == 0, '%s C4 planted twin (opposite phase value): %d of %d shots differ' % (name, len(bad2), len(shots)))
    pre = {k: (res[k].get('predrawn'), res[k].get('taken')) for k in ('a_on', 'a_flip', 'a_follow')}
    m = {k: ARENA_RE.search(res[k]['text']) for k in ('a_on', 'a_flip', 'a_follow')}
    adm = {k: (int(m[k].group(3)), int(m[k].group(2))) if m[k] else None for k in m}
    say(all(adm[k] is not None for k in adm) and adm['a_flip'][0] > 0, '%s C4 taken: texture-alpha proof admitted/refused: steady on %s, flipped (scan held) %s, flipped scan-follow with an OFF first phase %s; (prepass passes, draws pre-drawn): %s' % (name, adm['a_on'], adm['a_flip'], adm['a_follow'], pre))
    say(adm['a_follow'] is not None and adm['a_flip'] is not None and adm['a_follow'][0] < adm['a_flip'][0], '%s C4 scan hold matters: with the scan following the switch (an OFF first phase) the proof admits %s draws, with the scan held %s (the arena_scan_hold switch is what a flip job arms)' % (name, adm['a_follow'][0] if adm['a_follow'] else None, adm['a_flip'][0] if adm['a_flip'] else None))
    print('INFO %s scan-follow finding: admitted %s vs %s (held): textures that arrive while the switch is off stay unproven until their content arrives again' % (name, adm['a_follow'], adm['a_flip']), flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=cc.ROOT)
    ap.add_argument('--only')
    a = ap.parse_args()
    root = a.root
    main_co = cc.main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    outdir = os.path.join(root, 'out', 's102', 'r24d_flip')
    os.makedirs(outdir, exist_ok=True)
    for name in CAPTURES:
        if a.only and a.only != name:
            continue
        run_one(root, name, outdir, game, main_co)
    print('FAILED: %d' % len(FAILS) if FAILS else 'ALL PASS')
    sys.exit(1 if FAILS else 0)


if __name__ == '__main__':
    main()
