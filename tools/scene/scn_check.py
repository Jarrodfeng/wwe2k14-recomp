#!/usr/bin/env python3
r"""S103 lane SCN (B): an IDLE movie-key screen draws black, never white_ (0 game runs, offline rig).

  py -I tools\scene\scn_check.py [--capture C:\...\out\captures\a04_entrance] [--frame 100] [--plant] [--expect-fail] [--out DIR]

The game's idle stage screen (a movie surface with no movie) is black: f5401 of run 38 reads (1,1,1) where ours was (250,251,251) = the front end's white_ (the screen samplers
(titantron_movie, main&banner, ppv_header&wall, ss_header&wall, mini_tron) name no file in any texture directory, so the material has no diffuse and the front end bound white_).
The rig has no live picture feed, so every movie-key screen of the frame is idle there. The rig's feature dump (WWE_RDR_FEATURE_DUMP) names, per draw, what each sampler slot of the
pixel variant is bound to ("white" / "black" / "map"). Verdict, over the draws whose part is "slot 800..809":
  1. at least 3 movie-key draws exist (else the check proves nothing);
  2. NONE of them samples `white` on slot 0 (this fails the planted twin WWE_RDR_PLANT_IDLE_SCREEN_WHITE = rf::c::Plants::idle_screen_white, the old behaviour);
  3. every one of them samples a texture the front end names `map` (the loader's idle black texture is a real image) and NOT `none`.
--plant judges the planted twin with the same bars; --expect-fail turns that into the exit code (0 only when the bars FAIL it).
"""
import argparse, json, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))


def rig(capture, frame, out, plant):
    os.makedirs(out, exist_ok=True)
    dump = os.path.join(out, 'fdump_%s.jsonl' % ('plant' if plant else 'main'))
    if os.path.exists(dump):
        os.remove(dump)
    env = dict(os.environ)
    env['WWE_FVIEW_ALLOW_STALE'] = '1'
    env['WWE_RDR_FEATURE_DUMP'] = dump
    env.pop('WWE_RDR_PLANT_IDLE_SCREEN_WHITE', None)
    if plant:
        env['WWE_RDR_PLANT_IDLE_SCREEN_WHITE'] = '1'
    p = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'frame_rig.py'), '--capture', capture, '--frames', str(frame), '--repeat', '1', '--out', os.path.join(out, 'plant' if plant else 'main')],
                       capture_output=True, text=True, errors='replace', env=env, timeout=1800)
    if not os.path.exists(dump):
        print('FAIL  the rig wrote no feature dump: %s' % ((p.stdout or '')[-600:] + (p.stderr or '')[-300:]))
        sys.exit(1)
    return dump


def movie_draws(dump):
    rows = []
    for line in open(dump, encoding='utf-8', errors='replace'):
        try:
            d = json.loads(line)
        except ValueError:
            continue
        part = str(d.get('part', ''))
        if d.get('k') != 'draw' or not part.startswith('slot '):
            continue
        try:
            key = int(part.split()[1])
        except (ValueError, IndexError):
            continue
        if 800 <= key <= 809:
            rows.append((key, (d.get('slots') or {}).get('0', 'unbound')))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--capture', default=None)
    ap.add_argument('--frame', type=int, default=100)
    ap.add_argument('--plant', action='store_true')
    ap.add_argument('--expect-fail', action='store_true')
    ap.add_argument('--out', default=os.path.join(ROOT, 'out', 'scn'))
    a = ap.parse_args()
    cap = a.capture
    if not cap:
        sys.path.insert(0, HERE)
        import frame_rig
        cap = os.path.join(frame_rig.main_checkout(), 'out', 'captures', 'a04_entrance')
    rows = movie_draws(rig(cap, a.frame, a.out, a.plant))
    kinds = {}
    for key, kind in rows:
        kinds.setdefault(kind, []).append(key)
    print('  movie-key draws (slot 800..809) of the rig frame, slot-0 binding: %s' % {k: sorted(set(v)) for k, v in kinds.items()})
    fails = []
    if len(rows) < 3:
        print('FAIL  1: only %d movie-key draws (nothing to prove)' % len(rows))
        return 1
    print('PASS  1: %d movie-key draws' % len(rows))
    white = [k for k, v in rows if v == 'white']
    if white:
        fails.append('keys %s sample white_' % sorted(set(white)))
    print('%s  2: no idle movie-key screen samples white_ (%d do)' % ('FAIL' if white else 'PASS', len(white)))
    nonmap = [k for k, v in rows if v != 'map']
    if nonmap:
        fails.append('keys %s are not bound to a real image' % sorted(set(nonmap)))
    print('%s  3: every idle movie-key screen has the loader\'s black image bound (%d do not)' % ('FAIL' if nonmap else 'PASS', len(nonmap)))
    if a.expect_fail:
        print('%s  planted: %s' % ('PASS' if fails else 'FAIL', ('failed as it must (%s)' % '; '.join(fails)) if fails else 'did NOT fail'))
        return 0 if fails else 1
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
