r"""S102 lane M1: the count budget. Compares the renderer's counts file (rplay --counts, src/renderer/counts.h) with a committed baseline and fails when any
count RISES beyond the tolerance (default 0). Counts only: the baseline holds integers, no game data (hard rule 1).

  py -I tools\budget\budget_check.py --counts OUT.json --baseline tools\budget\bryan_entrance.json [--tolerance N] [--tolerance-pct P] [--accept-falls]
  py -I tools\budget\budget_check.py --capture DIR [--frames N] --baseline FILE [--twice] [--plant-pass K [--expect-fail]]   (runs this checkout's rplay itself)
  py -I tools\budget\budget_check.py --capture DIR [--frames N] --write-baseline FILE [--scene NAME]                            (makes / refreshes the baseline)

What is compared (each is a sum over the frames of the window unless it says max):
  frame fields   passes, steps, texture_sets, ring_constants, ring_geometry, ring_prev, upload_bytes, dropped
  total          every call count of the whole frame (draws, indexed_draws, dispatches, copies, clears, barriers, barrier_images, pipeline_binds,
                 descriptor_binds, push_constants, dynamic_sets, vertex_binds, index_binds, vertices, indices, area_px), summed AND the per-frame maximum
  per pass       the same call counts for each pass, summed over the frames. A pass is keyed by what it IS, not by its place in the frame (indices shift when
                 a pass comes or goes): its first target image + mip + layer, size and colour count, plus the occurrence of that key in the frame (counts files
                 without target fields fall back to size + colour count + occurrence). When the set of pass keys (and how many frames each is in) equals the
                 baseline's, a rise in any pass fails and names it. When the structure differs (a pass added / removed / re-targeted) the per-pass lines are
                 only reported ("changed") and the TOTALS decide.
Safeguards (exit 2, "cannot run", never a pass): an unreadable / short counts file, a frame without the `pass` key, any frame with 0 passes or 0 draws, a
frame-sum of passes or draws under 50 % of the baseline (a run that drew nothing; --accept-falls allows a deliberate drop), and, when rplay is run here,
a baseline made from another capture / window / rplay options (capture fingerprint = sizes of the three .bin files and the window's frame files).
A count below its baseline passes and is reported as an improvement ("re-baseline with --write-baseline"). A count above it fails and is named.
Exit 0 pass, 1 fail, 2 cannot run. --expect-fail inverts 0 / 1 (the planted-fault twin: exit 0 only when the budget FAILED naming pass K).
"""
import argparse, hashlib, json, os, subprocess, sys, tempfile
from collections import Counter
from pathlib import Path

CALL = ['draws', 'indexed_draws', 'dispatches', 'copies', 'clears', 'barriers', 'barrier_images', 'pipeline_binds', 'descriptor_binds', 'push_constants',
        'dynamic_sets', 'vertex_binds', 'index_binds', 'vertices', 'indices', 'area_px']
FRAME = ['passes', 'steps', 'texture_sets', 'ring_constants', 'ring_geometry', 'ring_prev', 'upload_bytes', 'dropped']
FRAME_REQUIRED = [k for k in FRAME if k != 'dropped']   # (a file from before the `dropped` field reads it as 0)
HERE = Path(__file__).resolve().parents[2]
FALL_FLOOR_PCT = 50


class Bad(Exception):
    pass


def load_counts(path):
    try:
        d = json.loads(Path(path).read_text(encoding='utf-8'))
    except Exception as e:
        raise Bad('cannot read counts file %s: %s' % (path, e))
    if d.get('format') != 'rdr-counts' or not isinstance(d.get('frames'), list) or not d['frames']:
        raise Bad('%s is not a counts file with frames (format %r)' % (path, d.get('format')))
    for i, f in enumerate(d['frames']):
        for k in FRAME_REQUIRED:
            if k not in f:
                raise Bad('frame %d of %s has no "%s"' % (i, path, k))
        if not isinstance(f.get('pass'), list):
            raise Bad('frame %d of %s has no "pass" list' % (i, path))
        for k in CALL:
            if k not in f.get('total', {}):
                raise Bad('frame %d of %s has no total "%s"' % (i, path, k))
        for p in f['pass']:
            for k in CALL + ['index', 'width', 'height']:
                if k not in p:
                    raise Bad('frame %d pass %r of %s has no "%s"' % (i, p.get('index'), path, k))
    return d


def pass_key(p, seen):
    """What the pass is: first target + size + colour count (+ the occurrence of that key in the frame)."""
    if 'target_image' in p:
        base = 'i%dm%dl%d_%dx%dc%d' % (p['target_image'], p.get('target_mip', 0), p.get('target_layer', 0), p['width'], p['height'], p.get('color_count', 0))
    else:
        base = '%dx%dc%d' % (p['width'], p['height'], p.get('color_count', 0))
    seen[base] += 1
    return '%s#%d' % (base, seen[base])


def summarise(d):
    """The numbers the budget compares: a plain dict (also what a baseline stores)."""
    frames = d['frames']
    s = {'frames': len(frames), 'frame': {k: sum(f.get(k, 0) for f in frames) for k in FRAME},
         'empty_frames': sum(1 for f in frames if not f['pass'] or f['total']['draws'] + f['total']['indexed_draws'] == 0),
         'total': {'sum': {k: sum(f['total'][k] for f in frames) for k in CALL}, 'max': {k: max(f['total'][k] for f in frames) for k in CALL}},
         'passes': {}}
    for f in frames:
        seen = Counter()
        for p in f['pass']:
            key = pass_key(p, seen)
            e = s['passes'].setdefault(key, {'label': 'index %d, %dx%d, first op %d' % (p['index'], p['width'], p['height'], p.get('first_op', 0)), 'n': 0, 'sum': {k: 0 for k in CALL}})
            e['n'] += 1
            for k in CALL:
                e['sum'][k] += p[k]
    return s


def allowed(base, tol_abs, tol_pct):
    return base + max(tol_abs, (base * tol_pct) // 100 if tol_pct else 0)


def compare(cur, base, tol_abs=0, tol_pct=0, accept_falls=False):
    """Returns (rises, falls): lists of text lines. A rise is a failure. Raises Bad for a run that cannot be judged (empty, wrong window)."""
    if cur['frames'] != base['frames']:
        raise Bad('frame count %d, the baseline has %d (run the same window)' % (cur['frames'], base['frames']))
    if cur['empty_frames']:
        raise Bad('%d of %d frames have 0 passes or 0 draws: an empty run is not a pass' % (cur['empty_frames'], cur['frames']))
    if not accept_falls:
        for label, c, b in (('passes', cur['frame']['passes'], base['frame']['passes']),
                            ('draws', cur['total']['sum']['draws'] + cur['total']['sum']['indexed_draws'], base['total']['sum']['draws'] + base['total']['sum']['indexed_draws'])):
            if b and c * 100 < b * FALL_FLOOR_PCT:
                raise Bad('frame-sum of %s is %d, under %d %% of the baseline %d: the run drew far less than the baseline (--accept-falls if deliberate)' % (label, c, FALL_FLOOR_PCT, b))
    rises, falls = [], []

    def one(label, c, b, into_rises=True):
        if c > allowed(b, tol_abs, tol_pct):
            (rises if into_rises else falls).append('%s: %d > %d (+%d)' % (label, c, b, c - b) if into_rises else 'changed %s: %d > %d (+%d)' % (label, c, b, c - b))
        elif c < b:
            falls.append('%s: %d < %d (%d lower)' % (label, c, b, b - c))
    for k in FRAME:
        one('frame sum %s' % k, cur['frame'][k], base['frame'].get(k, 0))
    for k in CALL:
        one('total sum %s' % k, cur['total']['sum'][k], base['total']['sum'][k])
        one('total max/frame %s' % k, cur['total']['max'][k], base['total']['max'][k])
    same = ({k: v['n'] for k, v in cur['passes'].items()} == {k: v['n'] for k, v in base['passes'].items()})
    if not same:
        gone = sorted(set(base['passes']) - set(cur['passes']))
        new = sorted(set(cur['passes']) - set(base['passes']))
        falls.append('pass structure differs from the baseline (%d passes gone, %d new): per-pass lines are reported as "changed", the totals decide' % (len(gone), len(new)))
    for key in sorted(cur['passes']):
        c = cur['passes'][key]
        b = base['passes'].get(key)
        name = 'pass %s (%s)' % (key, c['label'])
        if b is None:
            if not same and any(c['sum'][k] for k in CALL):
                falls.append('changed %s: new, %s' % (name, ', '.join('%s %d' % (k, c['sum'][k]) for k in CALL if c['sum'][k])))
            continue
        for k in CALL:
            one('%s sum %s' % (name, k), c['sum'][k], b['sum'][k], into_rises=same)
    for key in sorted(base['passes']):
        if key not in cur['passes']:
            falls.append('changed pass %s of the baseline is gone (%d draws there)' % (key, base['passes'][key]['sum']['draws'] + base['passes'][key]['sum']['indexed_draws']))
    return rises, falls


def load_baseline(path):
    try:
        b = json.loads(Path(path).read_text(encoding='utf-8'))
    except Exception as e:
        raise Bad('cannot read baseline %s: %s' % (path, e))
    if b.get('format') != 'rdr-budget' or b.get('version') != 2 or 'counts' not in b:
        raise Bad('%s is not a version 2 budget baseline (key passes by identity; refresh with --write-baseline)' % path)
    c = b['counts']
    for k in FRAME_REQUIRED:
        if k not in c.get('frame', {}):
            raise Bad('baseline %s has no frame "%s"' % (path, k))
    for k in CALL:
        if k not in c.get('total', {}).get('sum', {}) or k not in c['total'].get('max', {}):
            raise Bad('baseline %s has no total "%s"' % (path, k))
    if not c.get('passes'):
        raise Bad('baseline %s has no passes' % path)
    return b


def capture_meta(cap, frames):
    """The capture's fingerprint: sizes (not contents) of the three .bin files and of the window's frame files. Cheap, and changes if the capture or the window does."""
    h = hashlib.sha256()
    for name in ('images.bin', 'geometry.bin', 'textures.bin'):
        p = cap / name
        if not p.is_file():
            raise Bad('capture %s has no %s' % (cap, name))
        h.update(('%s %d\n' % (name, p.stat().st_size)).encode())
    fdir = cap / 'frames'
    files = sorted(fdir.glob('frame_*.bin')) if fdir.is_dir() else []
    if not files:
        raise Bad('capture %s has no frames' % cap)
    for f in files[:frames]:
        h.update(('%s %d\n' % (f.name, f.stat().st_size)).encode())
    return {'capture_fingerprint': h.hexdigest(), 'capture_frames': len(files), 'window_frames': frames, 'rplay_options': ['--frames', str(frames)]}


def check_meta(bm, meta, path='baseline'):
    if not bm:
        raise Bad('baseline %s has no capture fingerprint (made from a counts file); refresh it with --capture ... --write-baseline' % path)
    for k in ('capture_fingerprint', 'capture_frames', 'window_frames', 'rplay_options'):
        if bm.get(k) != meta[k]:
            raise Bad('the baseline was made from another capture / window / options (%s: baseline %r, now %r)' % (k, bm.get(k), meta[k]))


def run_rplay(capture, frames, out, plant, exe):
    cmd = [str(exe), '--capture', str(capture), '--frames', str(frames), '--counts', str(out)]
    if plant is not None:
        cmd += ['--counts-plant', str(plant)]
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    if p.returncode != 0:
        tail = '\n'.join((p.stdout + p.stderr).strip().splitlines()[-6:])
        raise Bad('rplay exit %d\n%s' % (p.returncode, tail))


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--counts')
    ap.add_argument('--baseline')
    ap.add_argument('--write-baseline')
    ap.add_argument('--scene', default='')
    ap.add_argument('--capture')
    ap.add_argument('--frames', type=int, default=100)
    ap.add_argument('--rplay', default=str(HERE / 'out' / 'renderer' / 'dev' / 'rplay.exe'))
    ap.add_argument('--twice', action='store_true', help='run rplay twice and require byte-identical counts files (prints the hash)')
    ap.add_argument('--plant-pass', type=int)
    ap.add_argument('--expect-fail', action='store_true')
    ap.add_argument('--accept-falls', action='store_true', help='allow the frame-sum of passes / draws to fall under half the baseline')
    ap.add_argument('--tolerance', type=int, default=0)
    ap.add_argument('--tolerance-pct', type=int, default=0)
    a = ap.parse_args(argv)
    try:
        counts_path = a.counts
        meta = None
        with tempfile.TemporaryDirectory(prefix='budget_') as tmp:
            if a.capture:
                main_co = Path(os.environ.get('RDR_MAIN_CHECKOUT', HERE))
                cap = Path(a.capture)
                if not cap.exists() and (main_co / a.capture).exists():
                    cap = main_co / a.capture
                if not cap.exists():
                    raise Bad('capture %s not found' % a.capture)
                if not Path(a.rplay).is_file():
                    raise Bad('rplay not found: %s' % a.rplay)
                meta = capture_meta(cap, a.frames)
                counts_path = str(Path(tmp) / 'counts.json')
                run_rplay(cap, a.frames, counts_path, a.plant_pass, a.rplay)
                if a.twice:
                    second = str(Path(tmp) / 'counts2.json')
                    run_rplay(cap, a.frames, second, a.plant_pass, a.rplay)
                    h1, h2 = hashlib.sha256(Path(counts_path).read_bytes()).hexdigest(), hashlib.sha256(Path(second).read_bytes()).hexdigest()
                    if h1 != h2:
                        raise Bad('two runs of the same capture gave different counts files (%s vs %s)' % (h1[:16], h2[:16]))
                    print('two runs identical: sha256 %s (%d bytes)' % (h1, Path(counts_path).stat().st_size))
            if not counts_path:
                raise Bad('give --counts FILE or --capture DIR')
            cur = summarise(load_counts(counts_path))
            if a.write_baseline:
                if cur['empty_frames']:
                    raise Bad('refusing to write a baseline from a run with %d empty frames' % cur['empty_frames'])
                out = {'format': 'rdr-budget', 'version': 2, 'scene': a.scene, 'meta': meta,
                       'note': 'counts only (src/renderer/counts.h); made by tools/budget/budget_check.py --write-baseline from rplay --counts. A count above these fails; refresh only for an intended, reviewed change.',
                       'counts': cur}
                Path(a.write_baseline).write_text(json.dumps(out, indent=1, sort_keys=True) + '\n', encoding='utf-8', newline='\n')
                print('wrote baseline %s: %d frames, %d passes' % (a.write_baseline, cur['frames'], len(cur['passes'])))
                return 0
            if not a.baseline:
                raise Bad('give --baseline FILE (or --write-baseline FILE)')
            base = load_baseline(a.baseline)
            if meta is not None:
                check_meta(base.get('meta'), meta, a.baseline)
            rises, falls = compare(cur, base['counts'], a.tolerance, a.tolerance_pct, a.accept_falls)
    except Bad as e:
        print('FAIL budget: %s' % e)
        return 2
    except subprocess.TimeoutExpired:
        print('FAIL budget: rplay timed out')
        return 2
    for line in falls[:20]:
        print('lower  ' + line)
    if len(falls) > 20:
        print('... and %d more lower / changed lines' % (len(falls) - 20))
    if falls:
        print('%d counts lower than the baseline or changed (improvements: refresh with --write-baseline after review)' % len(falls))
    for line in rises[:40]:
        print('RISE   ' + line)
    if len(rises) > 40:
        print('... and %d more' % (len(rises) - 40))
    print('%s budget: %d counts rose, %d fell, %d frames, %d passes' % ('FAIL' if rises else 'PASS', len(rises), len(falls), cur['frames'], len(cur['passes'])))
    if a.expect_fail:
        if not rises:
            print('FAIL expected the planted fault to be caught, but nothing rose')
            return 1
        if a.plant_pass is not None and not any(('(index %d,' % a.plant_pass) in r for r in rises):
            print('FAIL the planted fault in pass %d was caught but the pass was not named' % a.plant_pass)
            return 1
        print('PASS the planted fault was caught and named pass %s' % a.plant_pass)
        return 0
    return 1 if rises else 0


if __name__ == '__main__':
    sys.exit(main())
