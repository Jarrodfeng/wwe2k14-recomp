"""Screenshot references and the check against them (Session 8, task 16). Docs: docs/harness/screens.md.

References are game imagery: they live ONLY in out\\harness\\screenshots\\refs\\ (gitignored), with manifest.json
(route, label, run, the runs used for noise, settings, resolution, class, thresholds, A/A numbers). The maths is
tools/bench/shot_diff.py's (MAD over R, G, B; share of pixels with any channel off by > 32; a larger image is
downscaled to the smaller with Lanczos = the run-175 cross-resolution method).

    py tools\\bench\\shot_refs.py build ROUTE --runs 301,306,272 [--labels a,b] [--exclude c,d] [--note TEXT]
        Reference = the FIRST run's NNN_LABEL.bmp (copied). The other runs give the A/A noise (each vs the reference):
        class 'static' when no pixel of any A/A pair moved by more than 8 levels (MASK_LEVEL), else 'animated'. For
        animated labels a stable-pixel mask is learned (pixels whose max channel difference to the reference stays
        <= 8 in every run, the unstable area grown by 6 px per 720 lines) and checked tightly, while the whole frame
        gets a loose threshold. Without --labels every capture of the reference run is taken (a full rebuild),
        except the route's exclude list: --exclude sets it, it is kept in the manifest and applies to every later
        build of the route (--exclude "" clears it). With --labels only those labels are (re)built.
    py tools\\bench\\shot_refs.py check ROUTE 305[,306,...] [--labels REGEX] [--captures DIR] [--logs DIR]
        Each run vs the references; exit 1 if any label fails or is missing.
    py tools\\bench\\shot_refs.py frozen RUN LABEL_A LABEL_B
        Two captures of the same run must be identical (the pause menu's frozen 3D scene).
    py tools\\bench\\shot_refs.py show [ROUTE]
    --refs DIR (every command): another references folder (manifest.json inside), e.g. to try a build without
        touching the live references.

Thresholds (per label, from the build runs' A/A; numbers and rationale in docs/harness/screens.md):
    whole frame  static:   MAD <= max(1.5 x A/A max, 0.05) and off32 <= max(1.5 x A/A max, 0.01 %), and at the
                           same size off16 <= max(1.5 x A/A max, 0.01 %)
                 animated: MAD <= max(2 x A/A max, A/A max + 0.1), off32 <= max(2 x A/A max, A/A max + 0.05 %)
    mask         MAD <= max(1.5 x held-out max, MASK_FLOOR_MAD) and off32 <= max(1.5 x held-out max, MASK_FLOOR_OFF32)
                 and, at the same size, the count of masked pixels off by > 64 <= max(1.5 x held-out max,
                 MASK_FLOOR_N64 per 1280x720) (a missing icon or a few letters inside the mask)
    other size   + XRES_ALLOW_MAD / XRES_ALLOW_OFF32 on every threshold (resampling allowance, run 175), no off16,
                 no masked > 64 count (Lanczos ringing on sharp text)
'warn' (shown, does not fail) when a label passes but changed more than it ever did in A/A: a static label (no A/A
pixel moved by more than 8 levels) whose max level exceeds its A/A max level; an animated label whose largest
masked change exceeds max(2 x the held-out masked max level, 64). Same size only.
"""
import argparse
import datetime
import glob
import hashlib
import json
import os
import re
import shutil
import sys

import numpy as np
from PIL import Image, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import shot_diff  # noqa: E402

REPO = shot_diff.REPO
EXE_DIR = os.path.join(REPO, 'out', 'build', 'win-amd64-release')
DEFAULT_CAPTURES = os.path.join(EXE_DIR, 'captures')
DEFAULT_LOGS = os.path.join(EXE_DIR, 'logs')
REFS = os.path.join(REPO, 'out', 'harness', 'screenshots', 'refs')
MANIFEST = os.path.join(REFS, 'manifest.json')

K = 1.5                    # threshold = K x A/A max (or the floor)
ANIM_K = 2.0               # animated screens, whole frame: max(2 x A/A max, A/A max + margin)
# Margins sized to the noise: over 29 default selftest runs the all-pairs max is at most 0.04 points of off32 above
# the build runs' A/A max (storage_notice 0.066 % vs 0.026 %) and 0.06 of MAD (0.545 vs 0.492).
ANIM_MARGIN_MAD = 0.1
ANIM_MARGIN_OFF32 = 0.05   # percent points
FULL_FLOOR_MAD = 0.05
FULL_FLOOR_OFF32 = 0.01    # percent
FULL_FLOOR_OFF16 = 0.01    # percent, static screens at the same size only
# Chosen with the calibration runs in view (title run 180: masked MAD 0.197, sub-8 flicker), so not an independent
# held-out result; off32 and the > 64 count are the sharp tests.
MASK_FLOOR_MAD = 0.30
MASK_FLOOR_OFF32 = 0.05    # percent
MASK_N64_LEVEL = 64        # masked pixels off by more than this are counted ...
MASK_FLOOR_N64 = 25        # ... and at most this many per 1280x720 frame may be (0 in every default selftest run,
                           # 1 in the forced-slow-frame runs 275/290/291)
MASK_WARN_K = 2.0          # warn when the masked max level > max(2 x held-out max level, MASK_WARN_FLOOR)
MASK_WARN_FLOOR = 64
MASK_LEVEL = 8             # a pixel is stable if its max channel difference stays <= this in every build run
MASK_GROW_720 = 6          # unstable area grown by this many pixels per 720 lines
MASK_MIN_STABLE = 5.0      # percent of the frame; below this the mask is not used
XRES_ALLOW_MAD = 2.0       # added to every threshold when the run's size differs from the reference's
XRES_ALLOW_OFF32 = 0.5     # percent (run 175 4K logo screens vs 720p, Lanczos: MAD 0.83-1.11, off32 0.03-0.11 %)

CMDLINE_FLAG = re.compile(r"\] modified: (\w+) = '([^']*)' \(source: command line\)")
RUNLIST_FLAGS = {'user_data_root', 'cache_root', 'wwe2k14_timeline'}


def run_id(x):
    s = str(x).strip()
    if not s.isdigit():
        raise ValueError('run %r is not a run number (NNN, as in wwe2k14_NNN.log)' % s)
    return '%03d' % int(s)


def capture_path(captures, run, label):
    # Session 17: screenshots are PNG by default; older runs have BMPs.
    png = os.path.join(captures, '%s_%s.png' % (run, label))
    return png if os.path.exists(png) else os.path.join(captures, '%s_%s.bmp' % (run, label))


def run_settings(logs, run):
    """{flag: value} from the run's command-line 'modified:' lines (run_list's own flags left out)."""
    out = {}
    p = os.path.join(logs, 'wwe2k14_%s.log' % run)
    try:
        with open(p, encoding='utf-8', errors='replace') as f:
            for i, line in enumerate(f):
                if i > 400:
                    break
                m = CMDLINE_FLAG.search(line)
                if m and m.group(1) not in RUNLIST_FLAGS:
                    out[m.group(1)] = m.group(2)
    except OSError:
        return None
    return out


def run_identity(logs, run):
    """The run's log start time and its SDK pin line (logs\\sdk_pin_runs.log, written by run_list), when known."""
    ident = dict(start=None, sdk_pin=None)
    try:
        with open(os.path.join(logs, 'wwe2k14_%s.log' % run), encoding='utf-8', errors='replace') as f:
            m = re.match(r'\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)', f.readline())
            ident['start'] = m.group(1) if m else None
    except OSError:
        pass
    try:
        with open(os.path.join(logs, 'sdk_pin_runs.log'), encoding='utf-8', errors='replace') as f:
            for line in f:
                if line.startswith('wwe2k14_%s ' % run):
                    ident['sdk_pin'] = line.strip()
    except OSError:
        pass
    return ident


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def load_manifest(path=None):
    path = path or MANIFEST
    if not os.path.exists(path):
        return dict(version=1, routes={})
    with open(path, encoding='utf-8') as f:
        m = json.load(f)
    if not isinstance(m, dict) or not isinstance(m.get('routes'), dict):
        raise ValueError('%s has no "routes" table' % path)
    return m


def save_manifest(m, path=None):
    path = path or MANIFEST
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + '.tmp'
    with open(tmp, 'w', encoding='utf-8') as f:
        json.dump(m, f, indent=1)
    os.replace(tmp, path)


def diff_metrics(ref_path, run_path, mask_path=None):
    """Whole-frame and masked metrics of a run's capture against a reference. Sizes may differ (the larger is
    downscaled, see shot_diff.load_pair); the mask (white = stable, at the reference's size) is resized with it."""
    try:
        a, b, how, resized = shot_diff.load_pair(ref_path, run_path, cross_res=True)
    except ValueError as e:
        return dict(error=str(e))
    except OSError as e:
        return dict(error='unreadable: %s' % e)
    d = np.abs(a - b)
    dm = d.max(axis=2)
    res = dict(mad=float(d.mean()), off32=float((dm > shot_diff.OFF_LEVEL_HI).mean() * 100.0),
               off16=float((dm > shot_diff.OFF_LEVEL).mean() * 100.0), max=int(d.max()), size=how, resized=resized)
    if mask_path:
        try:
            with Image.open(mask_path) as im:
                mk = im.convert('L')
        except OSError as e:  # missing / unreadable mask: an error for this label, not an exception for the check
            return dict(error='mask %s unreadable: %s (rebuild the references)' % (os.path.basename(mask_path), e))
        h, w = dm.shape
        if mk.size != (w, h):
            # Conservative: a downscaled pixel is stable only if every source pixel was (BOX average == 255).
            mk = mk.resize((w, h), Image.BOX)
        m = np.asarray(mk) >= 255
        n = int(m.sum())
        res['mask_pct'] = 100.0 * n / m.size
        if n:
            res['m_mad'] = float(d[m].mean())
            res['m_off32'] = float((dm[m] > shot_diff.OFF_LEVEL_HI).mean() * 100.0)
            res['m_max'] = int(dm[m].max())
            res['m_n64'] = int((dm[m] > MASK_N64_LEVEL).sum())
    return res


def stable_mask(ref, others):
    """Bool array, True = stable in every run of `others` (max channel diff <= MASK_LEVEL vs ref), unstable grown."""
    unstable = np.zeros(ref.shape[:2], dtype=bool)
    for x in others:
        unstable |= np.abs(ref - x).max(axis=2) > MASK_LEVEL
    grow = max(1, int(round(MASK_GROW_720 * ref.shape[0] / 720.0)))
    img = Image.fromarray(np.where(unstable, 255, 0).astype(np.uint8)).filter(ImageFilter.MaxFilter(2 * grow + 1))
    return np.asarray(img) == 0


def masked_metrics(ref, x, stable):
    d = np.abs(ref - x)
    dm = d.max(axis=2)
    if not stable.any():
        return dict(m_mad=0.0, m_off32=0.0, m_max=0, m_n64=0)
    v = dm[stable]
    return dict(m_mad=float(d[stable].mean()), m_off32=float((v > shot_diff.OFF_LEVEL_HI).mean() * 100.0),
                m_max=int(v.max()), m_n64=int((v > MASK_N64_LEVEL).sum()))


def build_mask(ref_path, other_paths, out_path):
    """Writes the mask PNG (white = stable). -> (stable %, leave-one-out masked noise list).
    Leave-one-out: each run is measured inside a mask learned from the OTHER runs only, so the masked A/A numbers
    are held-out noise, not the zero the mask guarantees for its own runs."""
    ref = shot_diff.load(ref_path)
    others = []
    for p in other_paths:
        x = shot_diff.load(p)
        if x.shape != ref.shape:
            raise ValueError('mask runs must have the reference size: %s' % os.path.basename(p))
        others.append(x)
    stable = stable_mask(ref, others)
    Image.fromarray(np.where(stable, 255, 0).astype(np.uint8)).save(out_path)
    loo = []
    if len(others) >= 2:
        for i, x in enumerate(others):
            loo.append(masked_metrics(ref, x, stable_mask(ref, others[:i] + others[i + 1:])))
    return 100.0 * stable.mean(), loo


def thresholds(entry, resized):
    t = entry['thr']
    add_mad = XRES_ALLOW_MAD if resized else 0.0
    add_off = XRES_ALLOW_OFF32 if resized else 0.0
    full = dict(mad=t['full']['mad'] + add_mad, off32=t['full']['off32'] + add_off)
    if 'off16' in t['full'] and not resized:
        full['off16'] = t['full']['off16']
    masked = None
    if t.get('masked'):
        masked = dict(mad=t['masked']['mad'] + add_mad, off32=t['masked']['off32'] + add_off)
        if 'n64' in t['masked'] and not resized:  # pixel count at the reference's size; Lanczos rings when resized
            masked['n64'] = t['masked']['n64']
    return full, masked


def judge(entry, res):
    """-> (ok, text) for one label's diff_metrics against its manifest entry."""
    if 'error' in res:
        return False, res['error']
    full, masked = thresholds(entry, res['resized'])
    ok_full = res['mad'] <= full['mad'] and res['off32'] <= full['off32']
    txt = 'MAD %.3f (<= %.3f) off32 %.3f%% (<= %.3f%%)' % (res['mad'], full['mad'], res['off32'], full['off32'])
    if 'off16' in full:
        ok_full = ok_full and res['off16'] <= full['off16']
        txt += ' off16 %.3f%% (<= %.3f%%)' % (res['off16'], full['off16'])
    txt += ' max %d' % res['max']
    ok = ok_full
    if masked:
        if 'm_mad' not in res:
            return False, txt + '; mask: no stable pixel at this size'
        ok_m = res['m_mad'] <= masked['mad'] and res['m_off32'] <= masked['off32']
        n64 = ''
        if 'n64' in masked:
            ok_m = ok_m and res['m_n64'] <= masked['n64']
            n64 = ' >64: %d px (<= %d)' % (res['m_n64'], masked['n64'])
        ok = ok and ok_m
        txt += '; mask %.0f%%: MAD %.3f (<= %.3f) off32 %.3f%% (<= %.3f%%)%s max %d%s' % (
            res['mask_pct'], res['m_mad'], masked['mad'], res['m_off32'], masked['off32'], n64, res['m_max'],
            '' if ok_m else ' FAIL')
    txt += ' [%s, %s%s]' % (entry['class'], res['size'], ', cross-res allowance' if res['resized'] else '')
    return ok, txt


def check_run(route, run, captures=DEFAULT_CAPTURES, logs=DEFAULT_LOGS, labels=None, manifest=None):
    """-> (results, note). results: [(label, 'pass'|'fail', text)]; [] and a note when the route has no refs."""
    run = run_id(run)
    m = manifest if manifest is not None else load_manifest()
    r = m['routes'].get(route)
    if not r or not r.get('labels'):
        return [], 'no references for route %r in %s (build: py tools\\bench\\shot_refs.py build %s --runs N,N,...)' % (
            route, MANIFEST, route)
    out = []
    for label, entry in r['labels'].items():
        if labels and not re.search(labels, label):
            continue
        p = capture_path(captures, run, label)
        if not os.path.exists(p):
            out.append((label, 'fail', 'capture %s missing' % os.path.basename(p)))
            continue
        ref = os.path.join(REFS, entry['file'])
        if not os.path.exists(ref):
            out.append((label, 'fail', 'reference file %s missing (rebuild the references)' % ref))
            continue
        mask = os.path.join(REFS, entry['mask']) if entry.get('mask') else None
        if mask and not os.path.exists(mask):
            out.append((label, 'fail', 'mask file %s missing (rebuild the references)' % mask))
            continue
        res = diff_metrics(ref, p, mask)
        ok, txt = judge(entry, res)
        st = 'pass' if ok else 'fail'
        aa_level = entry.get('aa', {}).get('full', {}).get('max_level', 0)
        warn_level = (entry.get('thr', {}).get('masked') or {}).get('warn_level')
        if ok and entry['class'] == 'static' and not res['resized'] and res['max'] > aa_level:
            # Never moved by more than aa_level in any A/A pair, so a bigger change is news even under the
            # thresholds: shown, not failed.
            st = 'warn'
            txt += (' (static screen changed by up to %d levels, A/A max %d: under the thresholds, worth a look)'
                    % (res['max'], aa_level))
        elif ok and warn_level is not None and not res['resized'] and res.get('m_max', 0) > warn_level:
            # Same idea inside an animated screen's stable mask (a small change too few pixels to fail).
            st = 'warn'
            txt += (' (stable area changed by up to %d levels, warn above %d (held-out max %s): under the '
                    'thresholds, worth a look)' % (res['m_max'], warn_level,
                                                   entry['aa'].get('masked', {}).get('max_level', '?')))
        out.append((label, st, txt))
    note = None
    st = run_settings(logs, run)
    if st is not None:
        ref_st = next(iter(r['labels'].values())).get('settings') or {}
        diffs = sorted(set(k for k in set(st) | set(ref_st) if k != 'wwe2k14_input_script'
                           and str(st.get(k, '')).lower() != str(ref_st.get(k, '')).lower()))
        if diffs:
            note = 'settings differ from the reference run: ' + ', '.join(
                '%s=%s (ref %s)' % (k, st.get(k, '-'), ref_st.get(k, '-')) for k in diffs)
    return out, note


def frozen_check(run, label_a, label_b, captures=DEFAULT_CAPTURES, max_mad=0.05, max_off32=0.01):
    run = run_id(run)
    a, b = capture_path(captures, run, label_a), capture_path(captures, run, label_b)
    for p in (a, b):
        if not os.path.exists(p):
            return False, 'capture %s missing' % os.path.basename(p)
    res = shot_diff.compare(a, b)
    if 'error' in res:
        return False, res['error']
    ok = res['mad'] <= max_mad and res['off32'] <= max_off32
    return ok, '%s vs %s: MAD %.4f (<= %.2f) off32 %.3f%% (<= %.2f%%) max %d' % (
        label_a, label_b, res['mad'], max_mad, res['off32'], max_off32, res['max'])


def cmd_build(args):
    runs = []
    for x in args.runs.split(','):
        if x.strip() and run_id(x) not in runs:
            runs.append(run_id(x))
        elif x.strip():
            print('WARNING: run %s given twice: used once (a run is not its own A/A)' % run_id(x))
    if not runs:
        print('no runs given')
        return 1
    ref_run = runs[0]
    m = load_manifest()
    route = m['routes'].setdefault(args.route, dict(labels={}))
    if args.exclude is not None:  # given (even empty): replaces the route's exclude list
        route['exclude'] = sorted(set(x.strip() for x in args.exclude.split(',') if x.strip()))
    exclude = set(route.get('exclude') or [])
    labels = [x.strip() for x in args.labels.split(',') if x.strip()] if args.labels else sorted(
        os.path.basename(p)[len(ref_run) + 1:-4] for ext in ('bmp', 'png')
        for p in glob.glob(os.path.join(args.captures, ref_run + '_*.' + ext)))
    if exclude:
        skipped = [x for x in labels if x in exclude]
        labels = [x for x in labels if x not in exclude]
        print('route %s excludes %s (kept in the manifest; change with --exclude)%s' % (
            args.route, ','.join(sorted(exclude)), ('; skipped here: ' + ','.join(skipped)) if skipped else ''))
    if not labels:
        print('no captures %s_*.bmp in %s (after the exclude list)' % (ref_run, args.captures))
        return 1
    if not args.labels:
        route['labels'] = {}  # a full rebuild drops labels the reference run no longer has
    for x in list(route['labels']):
        if x in exclude:  # an excluded label never stays referenced
            del route['labels'][x]
    route['built'] = datetime.datetime.now().isoformat(timespec='seconds')
    route['note'] = args.note
    settings = run_settings(args.logs, ref_run)
    if settings is not None:
        route['script'] = os.path.basename(settings.get('wwe2k14_input_script', '').replace('\\', '/'))
    for r in runs[1:]:
        st = run_settings(args.logs, r)
        if st is not None and settings is not None and st != settings:
            print('WARNING: run %s settings differ from the reference run %s: %s vs %s (its noise is not A/A)' % (
                r, ref_run, json.dumps(st), json.dumps(settings)))
    os.makedirs(os.path.join(REFS, args.route), exist_ok=True)
    bad = 0
    for label in labels:
        src = capture_path(args.captures, ref_run, label)
        if shot_diff_problem(src):
            print('%-20s SKIP  reference capture %s: %s' % (label, os.path.basename(src), shot_diff_problem(src)))
            bad += 1
            continue
        with Image.open(src) as im:
            ref_size = im.size
        others = []
        for r in runs[1:]:
            p = capture_path(args.captures, r, label)
            prob = shot_diff_problem(p)
            if not prob:
                with Image.open(p) as im:
                    if im.size != ref_size:  # A/A noise only from same-size runs (and the mask needs them)
                        prob = 'size %dx%d differs from the reference\'s %dx%d' % (im.size + ref_size)
            if prob:
                print('%-20s       run %s left out: %s' % (label, r, prob))
                continue
            others.append((r, p))
        ext = os.path.splitext(capture_path(args.captures, ref_run, label))[1]  # .png (Session 17) or .bmp
        dst_rel = '%s/%s%s' % (args.route, label, ext)
        dst = os.path.join(REFS, args.route, label + ext)
        shutil.copyfile(src, dst)
        size = '%dx%d' % ref_size
        aa = [diff_metrics(dst, p) for _, p in others]
        aa = [x for x in aa if 'error' not in x]
        max_level = max([x['max'] for x in aa], default=0)
        # static: no A/A pixel moved by more than MASK_LEVEL (faint noise only, e.g. the pause scene's max 1)
        cls = 'static' if aa and max_level <= MASK_LEVEL else ('animated' if aa else 'no_aa')
        full_mad = max([x['mad'] for x in aa], default=0.0)
        full_off = max([x['off32'] for x in aa], default=0.0)
        full_off16 = max([x['off16'] for x in aa], default=0.0)
        entry = dict(file=dst_rel, run=ref_run, runs=[ref_run] + [r for r, _ in others], settings=settings,
                     resolution=size, sha256=sha256(dst), source=os.path.abspath(src), cls_note=None,
                     built=route['built'], run_identity=run_identity(args.logs, ref_run))
        entry['class'] = cls
        entry['aa'] = dict(n=len(aa), full=dict(mad_max=round(full_mad, 4), off32_max=round(full_off, 4),
                                                off16_max=round(full_off16, 4), max_level=max_level))
        entry['thr'] = dict(full=dict(mad=round(max(K * full_mad, FULL_FLOOR_MAD), 4),
                                      off32=round(max(K * full_off, FULL_FLOOR_OFF32), 4)), masked=None)
        if cls in ('static', 'no_aa'):
            # Same-size static screens also bound the pixels off by > 16 (antialiasing-sized edge changes; the
            # pause scene's no-MSAA run is 0.12 % here against 0 in A/A). Not applied to a resized pair.
            entry['thr']['full']['off16'] = round(max(K * full_off16, FULL_FLOOR_OFF16), 4)
        entry['mask'] = None
        if cls == 'animated':
            # The whole frame of a moving screen is only a gross-breakage test (the mask is the tight one): wider
            # than K x A/A, because its noise is not fully sampled by a few runs (run 312: storage_notice off32
            # 0.049 % against 1.5 x the build runs' 0.026 %; the all-pairs max over 29 default runs is 0.066 %).
            # The margin is sized to that gap (+0.05 points), not to the 0.5 points first used.
            entry['thr']['full'] = dict(
                mad=round(max(ANIM_K * full_mad, full_mad + ANIM_MARGIN_MAD, FULL_FLOOR_MAD), 4),
                off32=round(max(ANIM_K * full_off, full_off + ANIM_MARGIN_OFF32, FULL_FLOOR_OFF32), 4))
            mask_rel = '%s/%s.mask.png' % (args.route, label)
            stable, loo = build_mask(dst, [p for _, p in others], os.path.join(REFS, args.route, label + '.mask.png'))
            entry['mask_stable_pct'] = round(stable, 2)
            if stable >= MASK_MIN_STABLE:
                entry['mask'] = mask_rel
                # Held-out (leave-one-out) masked noise; with < 3 runs only the in-sample numbers (~0) exist.
                mm = loo or [diff_metrics(dst, p, os.path.join(REFS, mask_rel)) for _, p in others]
                m_mad = max([x.get('m_mad', 0.0) for x in mm], default=0.0)
                m_off = max([x.get('m_off32', 0.0) for x in mm], default=0.0)
                m_lvl = max([x.get('m_max', 0) for x in mm], default=0)
                m_n64 = max([x.get('m_n64', 0) for x in mm], default=0)
                entry['aa']['masked'] = dict(mad_max=round(m_mad, 4), off32_max=round(m_off, 4), max_level=m_lvl,
                                             n64_max=m_n64, how='leave-one-out' if loo else 'in-sample (too few runs)')
                n64_floor = int(round(MASK_FLOOR_N64 * ref_size[0] * ref_size[1] / (1280.0 * 720.0)))
                entry['thr']['masked'] = dict(mad=round(max(K * m_mad, MASK_FLOOR_MAD), 4),
                                              off32=round(max(K * m_off, MASK_FLOOR_OFF32), 4),
                                              n64=int(max(K * m_n64, n64_floor)),
                                              warn_level=int(max(MASK_WARN_K * m_lvl, MASK_WARN_FLOOR)))
        if cls == 'no_aa':
            entry['cls_note'] = 'only one usable run: no A/A, floors only (give more runs)'
        del_none = {k: v for k, v in entry.items() if v is not None or k in ('mask', 'cls_note')}
        route['labels'][label] = del_none
        t = entry['thr']
        print('%-20s %-8s ref %s %s, A/A n=%d full MAD max %.3f off32 max %.3f%% -> thr %.3f / %.3f%%%s' % (
            label, cls, ref_run, size, len(aa), full_mad, full_off, t['full']['mad'], t['full']['off32'],
            '' if not t.get('masked') else ('; mask %.0f%% stable, A/A MAD %.3f off32 %.3f%% max %d >64 %d px -> thr '
                                            '%.3f / %.3f%% / >64 %d px, warn above %d') % (
                entry['mask_stable_pct'], entry['aa']['masked']['mad_max'], entry['aa']['masked']['off32_max'],
                entry['aa']['masked']['max_level'], entry['aa']['masked']['n64_max'],
                t['masked']['mad'], t['masked']['off32'], t['masked']['n64'], t['masked']['warn_level'])))
    save_manifest(m)
    print('manifest %s (route %s: %d labels)' % (MANIFEST, args.route, len(route['labels'])))
    return 1 if bad else 0


def shot_diff_problem(p):
    if not os.path.exists(p):
        return 'missing'
    try:
        with Image.open(p) as im:
            im.load()
    except Exception as e:  # truncated / not an image
        return 'unreadable (%s)' % type(e).__name__
    return None


def cmd_check(args):
    runs = [run_id(x) for x in args.runs.split(',') if x.strip()]  # a bad run number stops here (exit 2)
    m = load_manifest()
    fails = 0
    for run in runs:
        res, note = check_run(args.route, run, args.captures, args.logs, args.labels, m)
        if not res:
            print('run %s: %s' % (run_id(run), note))
            fails += 1
            continue
        print('run %s vs %s references%s' % (run_id(run), args.route, ('  NOTE: ' + note) if note else ''))
        for label, st, txt in res:
            print('  %-20s %-4s %s' % (label, {'pass': 'ok', 'warn': 'warn'}.get(st, 'FAIL'), txt))
            fails += st == 'fail'
    print('%d failure(s)' % fails)
    return 1 if fails else 0


def cmd_frozen(args):
    ok, txt = frozen_check(args.run, args.label_a, args.label_b, args.captures)
    print('%s %s' % ('ok' if ok else 'FAIL', txt))
    return 0 if ok else 1


def cmd_show(args):
    m = load_manifest()
    for rn, r in m['routes'].items():
        if args.route and rn != args.route:
            continue
        print('%s (%s, built %s)%s' % (rn, r.get('script', '?'), r.get('built'), ('  ' + r['note']) if r.get('note') else ''))
        if r.get('exclude'):
            print('  excluded (never referenced, kept across rebuilds): %s' % ','.join(r['exclude']))
        for label, e in r['labels'].items():
            t = e['thr']
            tm = t.get('masked') or {}
            print('  %-20s %-8s ref %s %s runs %s  thr full MAD %.3f off32 %.3f%%%s%s  settings %s' % (
                label, e['class'], e['run'], e['resolution'], ','.join(e['runs']), t['full']['mad'], t['full']['off32'],
                ' off16 %.3f%%' % t['full']['off16'] if 'off16' in t['full'] else '',
                '' if not tm else '  mask(%.0f%%) MAD %.3f off32 %.3f%%%s%s' % (
                    e.get('mask_stable_pct', 0), tm['mad'], tm['off32'],
                    ' >64 %d px' % tm['n64'] if 'n64' in tm else '',
                    ' warn>%d' % tm['warn_level'] if 'warn_level' in tm else ''),
                json.dumps(e.get('settings'))))
    return 0


def main():
    global REFS, MANIFEST
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0], epilog=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument('--captures', default=DEFAULT_CAPTURES, help='captures folder (default: %(default)s)')
    common.add_argument('--logs', default=DEFAULT_LOGS, help='logs folder (run settings; default: %(default)s)')
    common.add_argument('--refs', default=REFS, help='references folder with manifest.json (default: %(default)s)')
    sub = ap.add_subparsers(dest='cmd', required=True)
    b = sub.add_parser('build', parents=[common], help='(re)build a route\'s references from runs (first = reference)')
    b.add_argument('route')
    b.add_argument('--runs', required=True, help='comma list; the first run is the reference, the rest give A/A')
    b.add_argument('--labels', default='', help='comma list (default: every capture of the reference run except '
                                                'the route\'s exclude list)')
    b.add_argument('--exclude', default=None, help='comma list of labels never referenced on this route; stored in '
                                                   'the manifest and kept for later builds ("" clears it)')
    b.add_argument('--note', default='')
    c = sub.add_parser('check', parents=[common], help='check runs against a route\'s references')
    c.add_argument('route')
    c.add_argument('runs', help='comma list of runs')
    c.add_argument('--labels', default='', help='regex: only these labels')
    f = sub.add_parser('frozen', parents=[common], help='two captures of one run must be identical')
    f.add_argument('run')
    f.add_argument('label_a')
    f.add_argument('label_b')
    s = sub.add_parser('show', parents=[common])
    s.add_argument('route', nargs='?')
    args = ap.parse_args()
    REFS = os.path.abspath(args.refs)
    MANIFEST = os.path.join(REFS, 'manifest.json')
    try:
        return dict(build=cmd_build, check=cmd_check, frozen=cmd_frozen, show=cmd_show)[args.cmd](args)
    except ValueError as e:  # bad run number, unreadable manifest
        print('ERROR: %s' % e)
        return 2


if __name__ == '__main__':
    sys.exit(main())
