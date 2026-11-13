r"""Lane 245 follow-up (reviewer item 5, the ring's RL1 lesson; docs\renderer\SKIP_GAME_WORK_MAP.md 16.9 / 18.9): `calls = runs` for the WRESTLERS, fed from the capture's RECORD instead of the emulated hook.

  py -I tools\d3\wrestler_calls_check.py CAPTURE [--frames A:B:STEP] [--rec-off 1] [--expect-clean]

Why. fview's EmulateHook (the offline `calls = runs` guard of d3_wrestler_check) feeds the hook tally from the SAME run grouping the binder uses, so the two agree by construction. The ring's first live verify run (RL1)
failed because the live hook counted 12 real calls of one model and the binder's ScanRuns merged their adjacent draws into ONE run. This tool counts REAL calls independently: every sub_82C04DF0 call writes one LightRef
(object, ordinal, state, hook frame) into the scene record (lane 109), so the number of LightRefs of an object in a hook frame is the number of real calls the game made (one keyed call per real call). It compares that with
the stream's keyed runs per object, three ways (the binder's ScanRuns merges adjacent draws of one object; a part-index restart splits an object's draws into its real calls):
  C      real calls of the object in the hook frame (LightRefs)
  R      runs as ScanRuns makes them: the object's keyed draws, one run per maximal consecutive stretch of the keyed-draw sequence
  S      runs split at a part restart (part index <= the previous draw's part of the same object): the real calls as the DRAWS show them
A wrestler object is `ok` when C == R (the live guard `calls = runs` then holds) and `split` when R < C == S (the ring's RL1 class: calls merged into one run). Hook frame = the record's header frame - rec-off (1).
Needs a capture with scene records (runs 7147 / 7149 / J1). 0 game runs, read only, numbers only. Exit 1 with --expect-clean when any wrestler object has C != R in any frame."""
import argparse
import collections
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'skip'))
import ledger as L  # noqa: E402
from ledger_io import CLASS_NAMES  # noqa: E402


def analyse(path, frames_spec='', rec_off=1):
    cap = L.Capture(path)
    if not cap.has_scene:
        raise L.LedgerError('this capture has no scene records (the live reader\'s records are the point of the tool)')
    reg = L.Registry(os.path.join(ROOT, 'src', 'renderer', 'shaders', 'spv', 'fx_registry.h'))
    frames = L.parse_frames(frames_spec, cap.n)
    # light refs by hook frame
    refs = collections.defaultdict(collections.Counter)
    recs = {}
    for i in range(cap.n):
        rec = cap.record(i)
        if rec is None:
            continue
        recs[i] = rec
        for obj, ordinal, state, frame in rec.light_refs():
            refs[frame][(obj, ordinal)] = 1
    calls_of = collections.defaultdict(lambda: collections.defaultdict(int))
    for fr, d in refs.items():
        for (obj, _ord) in d:
            calls_of[fr][obj] += 1
    res = {'wrestler': collections.Counter(), 'all': collections.Counter()}
    bad_frames = []
    for fi in frames:
        f = cap.frame(fi)
        rec = recs.get(fi)
        if rec is None or not f.has_origins:
            continue
        hook = rec.guest_frame - rec_off
        seq = []   # keyed draws in order: (object, part, is_wrestler)
        for i in range(f.ndraws):
            o = f.origin(i)
            if not o or CLASS_NAMES[o['klass']] != 'model_part' or o['pass_site'] not in (L.SITE_A, L.SITE_B):
                continue
            vn = reg.variant_of(f.draw(i)['vs'])
            seq.append((o['object'], o['part'], bool(vn and vn.startswith('yCh_'))))
        runs = collections.Counter()      # R
        splits = collections.Counter()    # S
        wr = {}
        prev_obj, prev_part = None, None
        for obj, part, is_wr in seq:
            wr[obj] = is_wr
            if obj != prev_obj:
                runs[obj] += 1
                splits[obj] += 1
            elif part <= prev_part:
                splits[obj] += 1
            prev_obj, prev_part = obj, part
        for obj in runs:
            c = calls_of.get(hook, {}).get(obj, 0)
            kind = 'wrestler' if wr[obj] else 'all'
            r, s = runs[obj], splits[obj]
            tag = 'ok' if c == r else ('split' if c == s else ('norecord' if c == 0 else 'other'))
            res[kind][tag] += 1
            if kind == 'wrestler' and tag != 'ok':
                bad_frames.append((fi, obj, c, r, s))
            if kind == 'wrestler':
                res[kind]['objects'] += 1
    return res, bad_frames, len(frames)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('capture')
    ap.add_argument('--frames', default='')
    ap.add_argument('--rec-off', type=int, default=1)
    ap.add_argument('--expect-clean', action='store_true')
    a = ap.parse_args()
    res, bad, n = analyse(a.capture, a.frames, a.rec_off)
    w = res['wrestler']
    print('frames %d; wrestler object-frames %d: calls == runs (ok) %d, merged into one run (split) %d, no record %d, other %d' % (n, w['objects'], w['ok'], w['split'], w['norecord'], w['other']))
    o = res['all']
    print('non-wrestler keyed objects (for scale; the ring class lives here): ok %d, merged (split) %d, no record %d, other %d' % (o['ok'], o['split'], o['norecord'], o['other']))
    for fi, obj, c, r, s in bad[:12]:
        print('  frame %d object %08X: real calls %d, ScanRuns runs %d, part-restart runs %d' % (fi, obj, c, r, s))
    if a.expect_clean and (w['split'] or w['other']):
        print('FAIL  a wrestler object has real calls != ScanRuns runs')
        return 1
    print('PASS' if not (w['split'] or w['other']) else 'INFO  see the lines above')
    return 0


if __name__ == '__main__':
    sys.exit(main())
