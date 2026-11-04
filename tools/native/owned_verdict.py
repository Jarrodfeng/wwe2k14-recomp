"""Session 80 (Jarrod: "1440p and is it on seems like the way to go"): the owned-renderer verdict for runs of
tools\\native\\owned_check.ps1 (or any runs of the same route with owned steps on and off).

    py -I tools\\native\\owned_verdict.py RUN [RUN ...]

Each run is read from its log: its resolution scale, its owned list (gpu_native_own_steps; '0' = off), its route and
replay. Every run with owned steps on is paired with a run with them off of the same scale, route and replay (the
newest one given), and judged:

  shots     every screenshot vs the off run's (same names). Session 82 (the owned frame, design doc section 5):
            a TOLERANCE by default (the design review's rule). Per shot, on d = the largest channel difference of
            each pixel: (1) the 99.9th percentile of d, (2) the largest mean of d over any 16x16 block, (3) the share
            of pixels with d > 4. Within tolerance = p99.9 <= 8 AND no block mean > 2 AND share <= noise + 0.3%,
            noise = the same share between two owned-off runs of the same route / scale / exe (a second off run
            given, or found among the neighbouring logs; none = noise 0, said in the output). All three numbers are
            printed; OVER names the limit that failed. "identical" is still printed for byte-identical shots.
            --exact: byte-identical only, as before (Session 80).
                                                                                      FAIL if OVER / differs / missing
            A difference heat map PNG per shot pair that is not identical (out/owned_check/heat/) and one picture
            sheet per run pair (on | off | heat map for every shot; out/owned_check/sheets/ON_vs_OFF.jpg).
  on        the census lines ("owned steps per frame"): share with draws into our
            images, the mean draws / copies left out per frame                       FAIL if never on
            listed draws taken vs refused                                             WARN if mostly refused
  inexact   pauses after which a frame may be wrong (a sampler refused, a copy run
            without our pixels, ...) and refused samplers                             FAIL if any
  exact     pauses whose draws were replayed (only ownership lost), per shader       listed (WARN)
  valid.    with --vulkan_validation_enabled=true: validation errors in the log         FAIL if any

--dir EXEDIR: the runs were made by another exe (owned_check.ps1 -Exe): its logs/ and captures/ are read from there.
--shots-only A B: only the shot comparison of two runs (A as "on", B as "off"), whatever their flags - for trying the
tolerance on any pair.
--ab A B [B2]: Session 89, an A/B of two arms that may both be "on" (e.g. cards on vs off, two switches): A's shots vs
B's with the same tolerance, noise from B2 (a second run of arm B) when given, and both arms' census side by side
(owned share, truth work, pauses, validation). Exit 1 only when a shot is OVER.
An "on" run is one with gpu_native_own_steps listed or gpu_native_owned_frame=true (logged as modified).

Prints one block per pair and "OWNED VERDICT PASS / WARN / FAIL"; exit code 0 / 0 / 1.
"""
import re
import sys
from collections import Counter
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
LOGS = ROOT / 'out' / 'build' / 'win-amd64-release' / 'logs'
CAPTURES = ROOT / 'out' / 'build' / 'win-amd64-release' / 'captures'
OUT = ROOT / 'out' / 'owned_check'
# Session 82 tolerance (design doc section 5, review result): per shot pair three numbers on the per-pixel largest
# channel difference d (0-255): p99.9 of d, the largest mean of d over any 16x16 block, and the share of pixels with
# d > TOL_SMALL. PASS when p99.9 <= TOL_P999, no block mean > TOL_BLOCK and share <= noise + TOL_SHARE, where noise is
# the same share measured between two runs of the OFF arm (0 when no second off run exists).
TOL_SMALL, TOL_P999, TOL_BLOCK, TOL_SHARE = 4, 8.0, 2.0, 0.003

# The pauses after which a frame may be wrong (owned_steps.cpp OwnedDisable reasons); all others replay the pass's
# draws into the truth images first (exact - only ownership is lost for 600 frames).
INEXACT = (
    'a sampler of its page reads it otherwise',
    'a binding of its page the substitution cannot take',
    'a sampler of its page reads it as another class',
    'a rectangle copy into its page it cannot take',
    'a copy of a pass only our image holds run as the game',
    'a truth draw over a pass only our image holds',
)
MODIFIED = re.compile(r"modified: (resolution_scale|gpu_native_own_steps|gpu_native_owned_frame|wwe2k14_input_script|"
                      r"wwe2k14_replay_play|vulkan_validation_enabled) = '([^']*)'")
OWNED_FRAME_SUMMARY = re.compile(r'\[native\]\[owned-frame\] frames (\d+)-(\d+): passes (\d+) owned (\d+) fallback (\d+)'
                                 r'(?: \(by reason: ([^)]*)\))?')
VALIDATION_ERROR = re.compile(r'VUID-|Validation Error')
# Session 82 (harness builder): the census inside owned_check - the ON arms of s82_match_shots_census.txt log two frames'
# per-pass lines; the verdict prints the scoreboard from them and the truth work per frame of the last 600-frame summary.
FRAME_LINE = re.compile(r'\[native\]\[owned-frame\] frame \d+ pass \d+:')
FRAME_PASS = re.compile(r'\[owned-frame\] frame (\d+) pass \d+: (owned|fallback)')
FLIP_LINE = re.compile(r'cvar gpu_native_owned_frame = (true|false) \(frame (\d+)\) -> ok')
FLIP_SLACK = 6   # frames around the route's own off-window in which an exact pause is the flip's, not the piece's
SHOT_LINE = re.compile(r"\[input\] line \d+: screenshot '[^']*' \(frame (\d+)\)")
LOG_CVAR_LINE = re.compile(r'cvar gpu_native_owned_frame_log = \d+ \(frame (\d+)\)')
# The owned frame's verdict (review fix): a run with gpu_native_owned_frame=true is judged by its own census, not only by the
# steps census (which an arm with the owned frame forced off still produces): FAIL without a summary / logged frame that covers
# the match, with nothing owned, or with an owned share below OWNED_SHARE_MIN over a window that is (nearly) all match frames.
OWNED_SHARE_MIN = 0.95
MATCH_WINDOW_MIN = 0.9
# A match window is SETTLED from this many frames after scene=match (the load, the intro and the fade come first: the first 600
# frames of the entrance replay's match are 89% owned, the quick route's frames from +300 are 99.9%). Only a window that is mostly
# settled match frames is judged against OWNED_SHARE_MIN; any other window is reported as information.
SETTLE_FRAMES = 300
SCENE_MATCH = re.compile(r'scene=match \(frame (\d+)\)')
# Session 89: routes with no match by design (owned_check -Route bryan): their owned share is information, not the 95%
# match bar. Any other route without a scene=match line is judged as before (review: key on the route, not on mf).
NO_MATCH_ROUTES = ('s89_bryan_shots.txt', 's89_bryan_census.txt', 's89_brock_census.txt')
TRUTH_WORK = re.compile(r'TRUTH WORK per frame: ([^\[;]*)')


def route_name(value):
    """The route's file name; the census variant counts as the plain quick route (same shots on the same frames)."""
    n = Path(value).name
    if n in ('s82_match_shots_census.txt', 's82_match_shots_census_on.txt'):
        return 's82_match_shots_fast.txt'
    # (Session 89: an entrance census route's ON version - same shots, + an owned-frame flip - counts as the route itself.)
    return n[:-len('_on.txt')] + '.txt' if n.startswith('s89_') and n.endswith('_census_on.txt') else n
PAUSE = re.compile(r'\[native\]\[owned\] frame (\d+): (ps|vs) ([0-9A-F]{16}) not owned for 600 frames \((.*)\)')
CENSUS = re.compile(r'owned steps per frame: draws into our images ([\d.]+) in passes [\d.]+ \(resumed [\d.]+\), '
                    r'their copies left out ([\d.]+).*?; refused ([\d.]+)')


def read_run(run):
    # (An exe's own runs are numbered from 001 and named with three digits at least.)
    log = next((p for p in (LOGS / f'wwe2k14_{run}.log', LOGS / f'wwe2k14_{run:03d}.log') if p.exists()), None)
    if log is None:
        sys.exit(f'run {run}: no log {LOGS / f"wwe2k14_{run}.log"}')
    info = {'run': run, 'scale': '1', 'steps': '', 'route': '', 'replay': '', 'pauses': [], 'census': [],
            'validation': False, 'validation_errors': 0, 'owned_frame': False, 'frame_summaries': [],
            'frame_lines': 0, 'truth_work': [], 'summaries': [], 'flips': [], 'shot_frames': [], 'log_cvar_frame': None, 'frame_passes': {}, 'match_frame': None,
            'prefix': str(run) if list(CAPTURES.glob(f'{run}_*.png')) else f'{run:03d}'}
    with open(log, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = MODIFIED.search(line)
            if m:
                key, value = m.groups()
                if key == 'resolution_scale':
                    info['scale'] = value
                elif key == 'gpu_native_own_steps':
                    info['steps'] = value
                elif key == 'wwe2k14_input_script':
                    info['route'] = route_name(value)
                elif key == 'vulkan_validation_enabled':
                    info['validation'] = value == 'true'
                elif key == 'gpu_native_owned_frame':
                    info['owned_frame'] = value == 'true'
                else:
                    info['replay'] = value
                continue
            if VALIDATION_ERROR.search(line):
                info['validation_errors'] += 1
            fl = FLIP_LINE.search(line)
            if fl:
                info['flips'].append((int(fl.group(2)), fl.group(1) == 'true'))
            sf = SHOT_LINE.search(line)
            if sf:
                info['shot_frames'].append(int(sf.group(1)))
            lc = LOG_CVAR_LINE.search(line)
            if lc and info['log_cvar_frame'] is None:
                info['log_cvar_frame'] = int(lc.group(1))
            if info['match_frame'] is None and 'scene=match' in line:
                sm = SCENE_MATCH.search(line)
                if sm:
                    info['match_frame'] = int(sm.group(1))
            if '[owned-frame]' in line:
                m = OWNED_FRAME_SUMMARY.search(line)
                if m:
                    info['frame_summaries'].append(m.groups())
                    tw = TRUTH_WORK.search(line)
                    if tw:
                        info['truth_work'].append((m.group(1), m.group(2), tw.group(1).strip()))
                    info['summaries'].append({'a': int(m.group(1)), 'b': int(m.group(2)), 'passes': int(m.group(3)),
                                              'owned': int(m.group(4)), 'fallback': int(m.group(5)),
                                              'truth': tw.group(1).strip() if tw else ''})
                elif FRAME_LINE.search(line):
                    info['frame_lines'] += 1
                    fp = FRAME_PASS.search(line)
                    if fp:
                        c = info['frame_passes'].setdefault(int(fp.group(1)), [0, 0])
                        c[0 if fp.group(2) == 'owned' else 1] += 1
                continue
            m = PAUSE.search(line)
            if m:
                info['pauses'].append((int(m.group(1)), f'{m.group(2)} {m.group(3)[:8]}', m.group(4)))
                continue
            m = CENSUS.search(line)
            if m:
                info['census'].append((float(m.group(1)), float(m.group(2)), float(m.group(3))))
    info['on'] = info['steps'] not in ('', '0') or info['owned_frame']
    pre = info['prefix'] + '_'
    info['shots'] = sorted(p.name[len(pre):] for p in CAPTURES.glob(f'{pre}*.png'))
    return info


def route_off_windows(on):
    """The frames in which the route itself had the owned frame off: from each `cvar gpu_native_owned_frame false` to the next
    `true` (or the end of the log), widened by FLIP_SLACK frames. (The ON census route flips it at +300 and again after the last
    shot; with the frame off OwnedCopyBox does not clamp the 320x184 copy of a 320x180 box and the list mode pauses the key.)"""
    wins, start = [], None
    for frame, value in sorted(on['flips']):
        if not value and start is None:
            start = frame
        elif value and start is not None:
            wins.append((start - FLIP_SLACK, frame + FLIP_SLACK))
            start = None
    if start is not None:
        wins.append((start - FLIP_SLACK, 10 ** 9))
    return wins


def judge_owned_frame(on):
    """(level, lines, chosen summary or None) for an ON run that has gpu_native_owned_frame=true."""
    lines = []
    level = 'PASS'
    mf = on['match_frame']

    def worse(lv):
        nonlocal level
        if {'PASS': 0, 'WARN': 1, 'FAIL': 2}[lv] > {'PASS': 0, 'WARN': 1, 'FAIL': 2}[level]:
            level = lv

    sums = on['summaries']
    if not sums and not on['frame_passes']:
        return 'FAIL', ['  owned frame: NO CENSUS - no summary and no logged frame in the log (was the owned frame on?)'], None
    # The renderer's frame numbers (summaries, census lines) against the route's guest frame numbers (scene=match, shots):
    # the logged census frames start right after the route's cvar line; a different offset means the two counts drifted.
    drift = None
    if on['log_cvar_frame'] is not None and on['frame_passes']:
        drift = min(on['frame_passes']) - on['log_cvar_frame']
        if not 0 <= drift <= 10:
            lines.append(f"  owned frame: the renderer's frame numbers differ from the route's by {drift} (census frame "
                         f"{min(on['frame_passes'])} vs the cvar at guest frame {on['log_cvar_frame']}): the match share below is unreliable")
            worse('WARN')

    def share_of(s):
        if mf is None:
            return None
        return max(0, s['b'] - max(s['a'], mf + SETTLE_FRAMES) + 1) / float(s['b'] - s['a'] + 1)

    if on['log_cvar_frame'] is not None and not on['frame_passes']:
        lines.append('  owned frame: THE CENSUS LOGGED NO FRAME although the route asked for one (the owned frame was off at that point?) FAIL')
        worse('FAIL')
    covering = [s for s in sums if mf is None or s['b'] >= mf]
    logged_after = [f for f in on['frame_passes'] if mf is None or f >= mf]
    if mf is not None and not covering and not logged_after:
        return 'FAIL', [f'  owned frame: NO MATCH CENSUS - no summary or logged frame at or after the match start (frame {mf})'], None
    chosen = covering[-1] if covering else None
    if chosen is not None:
        sh = share_of(chosen)
        if chosen['owned'] == 0:
            lines.append(f"  owned frame: NOTHING OWNED in the window {chosen['a']}-{chosen['b']} ({chosen['passes']} passes, "
                         f"{chosen['fallback']} fallback): the owned frame is not doing anything")
            worse('FAIL')
        else:
            frac = chosen['owned'] / float(max(1, chosen['passes']))
            if sh is None and on['route'] in NO_MATCH_ROUTES:
                # (Session 89: a route with no match - owned_check -Route bryan, an entrance only - has no settled match
                # window; the 95% bar is a match bar, so the share is information here, as for entrance-heavy windows.)
                lines.append(f"  owned frame (information, not judged: no scene=match in this route): {100.0 * frac:.1f}% of "
                             f"passes owned over frames {chosen['a']}-{chosen['b']}")
                if on['shot_frames'] and chosen['b'] < max(on['shot_frames']):
                    lines.append(f"  owned frame: that window ends at frame {chosen['b']}, before the shots (frames "
                                 f"{min(on['shot_frames'])}-{max(on['shot_frames'])}): the census does not describe the shot frames")
            elif sh >= MATCH_WINDOW_MIN:
                tag = f"{100.0 * frac:.1f}% of passes owned over {chosen['a']}-{chosen['b']}" + \
                      ('' if sh is None else f' ({100.0 * sh:.0f}% match frames)')
                if frac < OWNED_SHARE_MIN:
                    lines.append(f'  owned frame: {tag}: BELOW {100.0 * OWNED_SHARE_MIN:.0f}% FAIL')
                    worse('FAIL')
                else:
                    lines.append(f'  owned frame: {tag}')
            else:
                lines.append(f"  owned frame (information, not judged): {100.0 * frac:.1f}% of passes owned over frames {chosen['a']}-"
                             f"{chosen['b']}, of which only {100.0 * sh:.0f}% are settled match frames (from {SETTLE_FRAMES} frames "
                             f"after scene=match at {mf}); the {100.0 * OWNED_SHARE_MIN:.0f}% bar applies to a settled match window "
                             f"(entrances are not owned yet)")
        if on['log_cvar_frame'] is not None and on['shot_frames'] and chosen['b'] < max(on['shot_frames']) - 5:   # (census routes only: their final flip-off summary ends the window)
            lines.append(f"  owned frame: the last summary ends at frame {chosen['b']}, before the last shot (frame "
                         f"{max(on['shot_frames'])}): the end of the run is not covered WARN")
            worse('WARN')
    for f, (own, fb) in sorted(on['frame_passes'].items()):
        if (mf is None or f >= mf) and own == 0:
            lines.append(f'  owned frame: logged frame {f} has 0 owned passes FAIL')
            worse('FAIL')
    return level, lines, chosen


def print_scoreboard(on):
    """The scoreboard (tools/native/scoreboard.py) of the frames this ON run logged, indented under its block."""
    import subprocess
    r = subprocess.run([sys.executable, '-I', str(ROOT / 'tools' / 'native' / 'scoreboard.py'), str(on['run']),
                        '--dir', str(LOGS)], capture_output=True, text=True, encoding='utf-8', errors='replace')
    text = (r.stdout or '').strip() or (r.stderr or '').strip()
    print(f"  scoreboard of the {on['frame_lines']} logged pass lines (census):")
    for ln in text.splitlines():
        print('    ' + ln)


# Session 82 (harness builder): NVIDIA FLIP (open source, pinned flip-evaluator 1.7 in thirdparty\\flip\\site, gitignored;
# py -m pip install --no-deps --target thirdparty\\flip\\site flip-evaluator==1.7) as a perceptual number next to the
# tolerance numbers: per differing shot the mean and the max FLIP error (0 = identical to the eye, 1 = very visible; the
# reference is the OFF shot, 67 pixels per degree) and a FLIP heat map column in the sheets. It never changes PASS / FAIL.
FLIP_SITE = ROOT / 'thirdparty' / 'flip' / 'site'
_flip = None


def flip_stats(on_path, off_path):
    """(mean, max, heat rgb uint8) of FLIP between the ON shot (test) and the OFF shot (reference), or None when FLIP is
    not installed or the pictures differ in size."""
    global _flip
    if _flip is None:
        _flip = False
        try:
            sys.path.insert(0, str(FLIP_SITE))
            import flip_evaluator
            _flip = flip_evaluator
        except Exception:
            pass
    if not _flip:
        return None
    ref = np.asarray(Image.open(off_path).convert('RGB'), np.float32) / 255.0
    tst = np.asarray(Image.open(on_path).convert('RGB'), np.float32) / 255.0
    if ref.shape != tst.shape:
        return None
    emap, mean, _ = _flip.evaluate(ref, tst, 'LDR', inputsRGB=True, applyMagma=False, computeMeanError=True)
    e = np.asarray(emap, np.float32)
    if e.ndim == 3:
        e = e[:, :, 0]
    v = np.clip(e, 0.0, 1.0)
    heat = np.zeros(v.shape + (3,), np.uint8)   # black -> red -> yellow
    heat[..., 0] = (np.clip(v * 2.0, 0.0, 1.0) * 255).astype(np.uint8)
    heat[..., 1] = (np.clip(v * 2.0 - 1.0, 0.0, 1.0) * 255).astype(np.uint8)
    return float(mean), float(e.max()), heat


def heat_map(d):
    """Difference heat map, coloured by the tolerance bands: black = identical, blue = 1-4, orange = 5-8, red = over
    8 (the p99.9 limit)."""
    h = np.zeros(d.shape + (3,), np.uint8)
    h[(d >= 1) & (d <= TOL_SMALL)] = (40, 70, 200)
    h[(d > TOL_SMALL) & (d <= TOL_P999)] = (255, 170, 30)
    h[d > TOL_P999] = (255, 30, 30)
    return h


def block_mean_max(d):
    """The largest mean of d over any 16x16 block (the picture is cropped to whole blocks)."""
    hh, ww = (d.shape[0] // 16) * 16, (d.shape[1] // 16) * 16
    if not hh or not ww:
        return float(d.mean())
    blocks = d[:hh, :ww].astype(np.float32).reshape(hh // 16, 16, ww // 16, 16)
    return float(blocks.mean(axis=(1, 3)).max())


def load_diff(a, b):
    ia = np.asarray(Image.open(a).convert('RGB')).astype(np.int16)
    ib = np.asarray(Image.open(b).convert('RGB')).astype(np.int16)
    if ia.shape != ib.shape:
        return None
    return np.abs(ia - ib).max(axis=2)


def find_noise_run(off, known, skip=None):
    """A second owned-off run of the same scale / route / replay (a run in `known` first, else a scan of the
    neighbouring logs in the log folder - their first 800 lines hold the 'modified:' flags), with the same shots."""
    runs = find_off_runs(off, known, skip)
    return runs[0] if runs else None


def find_off_runs(off, known, skip=None):
    """Every other owned-off run of the same scale / route / replay (`known` first, then the neighbouring logs).
    Session 82: a replay's same-frame shot comes out in a few fixed versions from run to run, owned frame on or off
    (core builder, sdk_0bb3da55f runs 016-027: shot a in 2 versions, shot b in 3, the same bytes across arms) - an
    ON shot byte-identical to ANY off run's shot is a known version, not a difference."""
    def is_twin(r):
        # (Session 82: an off arm of the owned frame keeps the arc's listed steps - a run with the same steps and the
        # owned frame off is an off run too.)
        off_like = not r['owned_frame'] and (not r['on'] or r['steps'] == off['steps'])
        return (off_like and r["run"] != off["run"] and r["run"] != skip and r['scale'] == off['scale'] and
                r['route'] == off['route'] and r['replay'] == off['replay'] and set(off['shots']) <= set(r['shots']))
    found = [r for r in known if is_twin(r)]
    for log in sorted(LOGS.glob('wwe2k14_*.log')):
        m = re.search(r'wwe2k14_(\d+)[.]log', log.name)
        if not m or abs(int(m.group(1)) - off['run']) > 40 or int(m.group(1)) == off['run']:
            continue
        head = {}
        try:
            with open(log, encoding='utf-8', errors='replace') as f:
                for i, line in enumerate(f):
                    if i > 800:
                        break
                    mm = MODIFIED.search(line)
                    if mm:
                        head[mm.group(1)] = mm.group(2)
        except OSError:
            continue
        if (head.get('resolution_scale', '1') == off['scale'] and
                route_name(head.get('wwe2k14_input_script', '')) == off['route'] and
                head.get('wwe2k14_replay_play', '') == off['replay'] and
                head.get('gpu_native_owned_frame') != 'true'):
            # (Session 82: whether it is an off run is read_run's call - the arc defaults list owned steps in the
            # owned frame's off arms too; is_twin checks it.)
            if any(f['run'] == int(m.group(1)) for f in found):
                continue
            r = read_run(int(m.group(1)))
            if is_twin(r):
                found.append(r)
    return found


def compare_shots(on, off, exact=False, known=(), twin=None, search=True):
    """Per shot: identical / within tolerance / OVER (or, with exact, DIFFERS). Returns (bad, lines, rows, identical,
    within, noise note); rows feed the picture sheet: (name, on path, off path, heat array or None, note).
    twin (Session 89, --ab): the second run of the B arm to take the noise from, instead of searching owned-off runs.
    search=False (--ab): never search the log folder for other runs (review: with no B2 an unrelated neighbouring run,
    even another A run, became the "known version" and passed every shot)."""
    lines, rows, bad, ident, within = [], [], 0, 0, 0
    others = [] if exact else ([twin] if twin else (find_off_runs(off, known, on["run"]) if search else []))
    twin = others[0] if others else None
    noise_note = ''
    if not exact:
        noise_note = (f"  noise: share over {TOL_SMALL} between reference runs {off['run']} and {twin['run']}" if twin else
                      "  noise: no second owned-off run of the same route / scale / exe in the log folder - noise = 0")
    for name in sorted(set(on['shots']) | set(off['shots'])):
        a, b = CAPTURES / f"{on['prefix']}_{name}", CAPTURES / f"{off['prefix']}_{name}"
        if not a.exists() or not b.exists():
            lines.append(f'    {name}: missing in {on["run"] if not a.exists() else off["run"]}')
            bad += 1
            continue
        d = load_diff(a, b)
        if d is None:
            lines.append(f'    {name}: sizes differ')
            bad += 1
            continue
        if not d.any():
            ident += 1
            rows.append((name, a, b, None, 'identical'))
            continue
        # (A known version: byte-identical to another off run's shot of the same name.)
        same = next((o for o in others if (CAPTURES / f"{o['prefix']}_{name}").exists() and
                     (dd := load_diff(a, CAPTURES / f"{o['prefix']}_{name}")) is not None and not dd.any()), None)
        if same:
            ident += 1
            lines.append(f"    {name}: identical to off run {same['run']} (a known version of this frame; "
                         f"differs from off run {off['run']})")
            rows.append((name, a, CAPTURES / f"{same['prefix']}_{name}", None, f"identical to off run {same['run']}"))
            continue
        share = float((d > TOL_SMALL).mean())
        p999 = float(np.percentile(d, 99.9))
        blk = block_mean_max(d)
        noise = 0.0
        if twin and (CAPTURES / f"{twin['prefix']}_{name}").exists():
            nd = load_diff(CAPTURES / f"{off['prefix']}_{name}", CAPTURES / f"{twin['prefix']}_{name}")
            noise = float((nd > TOL_SMALL).mean()) if nd is not None else 0.0
        heat = heat_map(d)
        OUT.joinpath('heat').mkdir(parents=True, exist_ok=True)
        hp = OUT / 'heat' / f"{on['run']}_vs_{off['run']}_{Path(name).stem}.png"
        Image.fromarray(heat).save(hp)
        nums = (f'p99.9 {p999:.1f}, worst 16x16 block mean {blk:.2f}, {100.0 * share:.3f}% of pixels over '
                f'{TOL_SMALL} (noise {100.0 * noise:.3f}%), max {int(d.max())}')
        short = 'p99.9 %.1f blk %.2f share %.3f%%' % (p999, blk, 100 * share)
        fl = flip_stats(a, b)
        fheat = None
        if fl:
            nums += f', FLIP mean {fl[0]:.4f} max {fl[1]:.3f}'
            short += ' FLIP %.4f/%.3f' % (fl[0], fl[1])
            fheat = fl[2]
        if exact:
            lines.append(f'    {name}: DIFFERS - {nums}; heat map {hp}')
            bad += 1
            rows.append((name, a, b, heat, 'DIFFERS: ' + short, fheat))
            continue
        failed = []
        if p999 > TOL_P999:
            failed.append(f'p99.9 {p999:.1f} > {TOL_P999:g}')
        if blk > TOL_BLOCK:
            failed.append(f'16x16 block mean {blk:.2f} > {TOL_BLOCK:g}')
        if share > noise + TOL_SHARE:
            failed.append(f'share over {TOL_SMALL} {100.0 * share:.3f}% > noise {100.0 * noise:.3f}% + '
                          f'{100.0 * TOL_SHARE:.1f}%')
        if failed:
            lines.append(f'    {name}: OVER ({"; ".join(failed)}) - {nums}; heat map {hp}')
            bad += 1
            rows.append((name, a, b, heat, 'OVER: ' + short, fheat))
        else:
            within += 1
            lines.append(f'    {name}: within tolerance - {nums}; heat map {hp}')
            rows.append((name, a, b, heat, 'within tolerance: ' + short, fheat))
    if not on['shots'] and not off['shots']:
        lines.append('    no screenshots in either run')
        bad += 1
    return bad, lines, rows, ident, within, noise_note


def make_sheet(on, off, rows, exact):
    """One JPEG per run pair: for every shot, owned on | owned off | heat map, labelled."""
    from PIL import ImageDraw
    if not rows:
        return None
    tw = 480
    tiles = []
    for name, a, b, heat, note, *rest in rows:
        ia, ib = Image.open(a).convert('RGB'), Image.open(b).convert('RGB')
        th = max(1, round(ia.height * tw / ia.width))
        hm = Image.fromarray(heat) if heat is not None else Image.new('RGB', ia.size, (0, 0, 0))
        fh = Image.fromarray(rest[0]) if rest and rest[0] is not None else Image.new('RGB', ia.size, (0, 0, 0))
        labels = (f"{name}  ON run {on['run']}", f"OFF run {off['run']}", f'difference: {note}',
                  'FLIP error (black 0, red, yellow 1)')
        for k, im in enumerate((ia, ib, hm, fh)):
            im = im.resize((tw, th), Image.NEAREST if k >= 2 else Image.LANCZOS)
            dr = ImageDraw.Draw(im)
            box = dr.textbbox((0, 0), labels[k])
            dr.rectangle((0, 0, box[2] + 8, box[3] + 6), fill=(0, 0, 0))
            dr.text((4, 2), labels[k], fill=(255, 255, 0))
            tiles.append(im)
    rowh = [max(tiles[i].height for i in range(r * 4, r * 4 + 4)) for r in range(len(rows))]
    head = 30
    sheet = Image.new('RGB', (4 * tw + 12, head + sum(rowh) + 4 * len(rows)), (40, 40, 40))
    dr = ImageDraw.Draw(sheet)
    rule = ('byte-identical required' if exact else f'tolerance: p99.9 <= {TOL_P999:g}, 16x16 block mean <= {TOL_BLOCK:g}, share over {TOL_SMALL} <= noise + {100.0 * TOL_SHARE:.1f}%')
    dr.text((6, 8), f"owned on run {on['run']} | owned off run {off['run']} | heat map (black = same, blue 1-{TOL_SMALL}, "
                    f"orange {TOL_SMALL + 1}-{TOL_P999:g}, red over {TOL_P999:g}) | FLIP heat map; {rule}", fill=(255, 255, 255))
    y = head
    for r in range(len(rows)):
        for c in range(4):
            sheet.paste(tiles[r * 4 + c], (c * (tw + 4), y))
        y += rowh[r] + 4
    OUT.joinpath('sheets').mkdir(parents=True, exist_ok=True)
    path = OUT / 'sheets' / f"{on['run']}_vs_{off['run']}.jpg"
    sheet.save(path, 'JPEG', quality=88)
    return path


OVER_FILE = OUT / 'over_sheets.txt'
OVER_QUESTION = (
    'Open the sheet. Columns left to right: owned ON | owned OFF | tolerance heat map (blue 1-4, orange 5-8, red over 8) | FLIP '
    'error (black = same to the eye, red / yellow = visible). For each of the shots named below say in ONE line what is visibly '
    'different between ON and OFF (where in the picture, what kind: brightness, missing or extra object, noise, banding, '
    'texture, shadow), and whether you would see it at normal size; write "not visible" if you cannot see it. Text only.')


def queue_over_sheet(on, off, rows, sheet, exact):
    """Session 82 (harness builder): a pair with a shot OVER tolerance gets its sheet and a fixed question appended to
    out/owned_check/over_sheets.txt - a script cannot call an agent, so the main session (or the builder) hands the entry to the
    shot-checker agent (Sonnet, low effort) and gets one line per shot back instead of looking at the images itself.
    Returns the number of OVER shots queued."""
    over = [(name, note) for name, a, b, heat, note, *rest in rows if note.startswith('OVER')]
    if exact or not over:
        return 0
    import datetime
    OUT.mkdir(parents=True, exist_ok=True)
    with open(OVER_FILE, 'a', encoding='utf-8') as f:
        f.write(f"== {datetime.datetime.now():%Y-%m-%d %H:%M:%S} ON run {on['run']} vs OFF run {off['run']} "
                f"(scale {on['scale']}, route {on['route']}, replay {on['replay'] or '-'})\n")
        f.write(f'sheet: {sheet}\n')
        for name, note in over:
            f.write(f'  {name}: {note}\n')
        f.write(f'QUESTION: {OVER_QUESTION}\n\n')
    return len(over)


def shots_report(on, off, exact, known=(), twin=None, off_label='owned off', search=True):
    """The shot lines + sheet for one pair; returns (bad, text lines)."""
    bad, lines, rows, ident, within, noise_note = compare_shots(on, off, exact, known, twin, search)
    head = f"  shots vs {off['run']} ({off_label}): "
    if bad:
        head += f'{bad} ' + ('FAIL' if exact else 'OVER / FAIL')
        head += f', {ident} identical' + ('' if exact else f', {within} within tolerance')
    else:
        head += f'{ident} identical' + ('' if exact else f', {within} within tolerance')
    out = [head] + ([noise_note] if noise_note else []) + lines
    sheet = make_sheet(on, off, rows, exact)
    if sheet:
        out.append(f'  sheet: {sheet}')
        queued = queue_over_sheet(on, off, rows, sheet, exact)
        if queued:
            out.append(f'  over: {queued} shot(s) OVER tolerance - sheet + question for the shot-checker agent added to {OVER_FILE}')
    return bad, out


def census_line(r):
    """One line of what a run's owned frame did (Session 89, --ab): the last 600-frame summary, truth work, pauses,
    validation errors."""
    parts = []
    if r['summaries']:
        s = r['summaries'][-1]
        share = 100.0 * s['owned'] / s['passes'] if s['passes'] else 0.0
        parts.append(f"frames {s['a']}-{s['b']}: passes {s['passes']} owned {s['owned']} ({share:.2f}%) fallback {s['fallback']}")
        if s['truth']:
            parts.append(f"truth work {s['truth']}")
    else:
        parts.append('no owned-frame summary')
    inexact = sum(1 for p in r['pauses'] if any(x in p[2] for x in INEXACT))
    parts.append(f"pauses {len(r['pauses'])} ({inexact} inexact)")
    if r['validation']:
        parts.append(f"validation errors {r['validation_errors']}")
    return '; '.join(parts)


def ab_report(runs, exact):
    """Session 89 (Session 87 compared two "on" arms by hand and missed the block rule): --ab A B [B2] judges arm A's
    shots against arm B's with the same tolerance as on vs off, the noise taken from B2 (a second run of arm B) when
    given, and prints both arms' census side by side. Neither arm has to be 'off'. FAIL only on shots."""
    if len(runs) not in (2, 3):
        sys.exit('--ab takes A B [B2]: two runs (arm A, arm B) and optionally a second run of arm B for the noise')
    a, b = runs[0], runs[1]
    twin = runs[2] if len(runs) == 3 else None
    if (a['scale'], a['route'], a['replay']) != (b['scale'], b['route'], b['replay']):
        print(f"  NOTE: the arms differ in scale / route / replay: {a['scale']}/{a['route']}/{a['replay'] or '-'} vs "
              f"{b['scale']}/{b['route']}/{b['replay'] or '-'}")
    print(f"== A/B: arm A run {a['run']} vs arm B run {b['run']}" + (f" (noise: B's second run {twin['run']})" if twin else
                                                                    ' (no second B run: noise 0)'))
    bad, out = shots_report(a, b, exact, runs, twin=twin, off_label='arm B', search=False)
    if not twin:
        out = [l for l in out if 'noise: no second owned-off run' not in l]
    print('\n'.join(out))
    print(f"  census A {a['run']}: {census_line(a)}")
    print(f"  census B {b['run']}: {census_line(b)}")
    if twin:
        print(f"  census B {twin['run']}: {census_line(twin)}")
    print(f"A/B SHOTS {'FAIL' if bad else 'PASS'}")
    return 1 if bad else 0


def main():
    global LOGS, CAPTURES
    args = sys.argv[1:]
    exact = '--exact' in args
    shots_only = '--shots-only' in args
    args = [a for a in args if a not in ('--exact', '--shots-only')]
    if '--dir' in args:
        i = args.index('--dir')
        exe = Path(args[i + 1])
        exe = exe if exe.is_absolute() else ROOT / exe
        LOGS, CAPTURES = exe / 'logs', exe / 'captures'
        del args[i:i + 2]
    ab = '--ab' in args
    args = [a for a in args if a != '--ab']
    runs = [read_run(int(a)) for a in args]
    if not runs:
        sys.exit(__doc__)
    if ab:
        return ab_report(runs, exact)
    if shots_only:
        if len(runs) != 2:
            sys.exit('--shots-only takes exactly two runs: ON OFF')
        bad, out = shots_report(runs[0], runs[1], exact, runs)
        print(f"== run {runs[0]['run']} vs {runs[1]['run']} ({'exact' if exact else 'tolerance'})")
        print('\n'.join(out))
        print(f"SHOTS {'FAIL' if bad else 'PASS'}")
        return 1 if bad else 0
    worst = 'PASS'
    rank = {'PASS': 0, 'WARN': 1, 'FAIL': 2}
    for on in (r for r in runs if r['on']):
        same = [r for r in runs if not r['on'] and r['scale'] == on['scale'] and r['route'] == on['route'] and
                r['replay'] == on['replay']]
        scale = '1440p' if on['scale'] == '2' else ('720p' if on['scale'] == '1' else f"scale {on['scale']}")
        print(f"== run {on['run']} ({scale}, {on['route']}, replay {on['replay'] or '-'})")
        verdict = 'PASS'
        if not same:
            print('  shots: no owned-off run of the same scale / route / replay given')
            verdict = 'FAIL'
        else:
            off = max(same, key=lambda r: r['run'])
            bad, out = shots_report(on, off, exact, runs)
            print('\n'.join(out))
            if bad:
                verdict = 'FAIL'
        if on['frame_summaries']:
            s = on['frame_summaries'][-1]
            print(f"  owned-frame census (last summary, frames {s[0]}-{s[1]}): passes {s[2]} owned {s[3]} "
                  f"fallback {s[4]}" + (f" (by reason: {s[5]})" if s[5] else ''))
        if on['frame_passes']:
            fr = sorted(on['frame_passes'])
            print('  logged frames (per-pass census; a fallback pass = a truth pass recorded): ' +
                  '; '.join(f"frame {f}: {sum(on['frame_passes'][f])} passes, {on['frame_passes'][f][0]} owned, "
                            f"{on['frame_passes'][f][1]} fallback" for f in fr) +
                  (f" (scene=match began at frame {on['match_frame']})" if on['match_frame'] else ''))
        if on['owned_frame']:
            of_level, of_lines, of_sum = judge_owned_frame(on)
            if of_sum is not None and of_sum['truth']:
                mf = on['match_frame']
                sh = None if mf is None else max(0, of_sum['b'] - max(of_sum['a'], mf + SETTLE_FRAMES) + 1) / float(of_sum['b'] - of_sum['a'] + 1)
                print(f"  truth work per frame (summary of frames {of_sum['a']}-{of_sum['b']}"
                      f"{'' if sh is None else f'; {100.0 * sh:.0f}% of these frames are settled match frames'}): {of_sum['truth']}")
            for ln in of_lines:
                print(ln)
            if of_level == 'FAIL':
                verdict = 'FAIL'
            elif of_level == 'WARN' and verdict == 'PASS':
                verdict = 'WARN'
        if on['frame_lines']:
            print_scoreboard(on)
        census = on['census']
        active = [c for c in census if c[0] > 0]
        if census:
            print(f'  on: {len(active)} / {len(census)} census lines with draws into our images '
                  f'({100.0 * len(active) / len(census):.0f}%); mean per frame there: draws '
                  f'{sum(c[0] for c in active) / max(1, len(active)):.1f}, copies left out '
                  f'{sum(c[1] for c in active) / max(1, len(active)):.1f}')
        else:
            print('  on: no census lines in the log')
        if not active:
            print('  on: NEVER ON - no draw went into our images')
            verdict = 'FAIL'
        # (Session 80, the phone's render path: steps "on" but ~1 draw a frame taken, ~150 refused - not a wrong
        # picture, but most of the listed work not owned: say so.)
        drawn, refused = sum(c[0] for c in census), sum(c[2] for c in census)
        if census:
            print(f'  listed draws per frame: {drawn / len(census):.1f} into our images, {refused / len(census):.1f} '
                  f'refused')
        if active and refused > drawn:
            print('  on: MOSTLY REFUSED - more listed draws refused than taken (the "refused (...)" log lines say why)')
            if verdict == 'PASS':
                verdict = 'WARN'
        inexact = [p for p in on['pauses'] if p[2].startswith(INEXACT)]
        exact = [p for p in on['pauses'] if not p[2].startswith(INEXACT)]
        wins = route_off_windows(on)
        flip_caused = [p for p in exact if any(a <= p[0] <= b for a, b in wins)]
        exact = [p for p in exact if p not in flip_caused]
        if flip_caused:
            print(f"  route flip (expected, not counted): {len(flip_caused)} exact pause(s) at frame(s) "
                  f"{', '.join(str(p[0]) for p in flip_caused)} inside the route's own owned-frame off-window(s) "
                  + ', '.join(f'{a + FLIP_SLACK}-{b - FLIP_SLACK if b < 10 ** 8 else "end"}' for a, b in wins)
                  + ': ' + '; '.join(sorted({f'{p[1]} ({p[2]})' for p in flip_caused})))
        if inexact:
            verdict = 'FAIL'
            print(f'  inexact pauses: {len(inexact)} FAIL (a frame may be wrong)')
            for (who, why), n in sorted(Counter((p[1], p[2]) for p in inexact).items()):
                first = min(p[0] for p in inexact if p[1] == who and p[2] == why)
                print(f'    {n} x {who} ({why}), first at frame {first}')
        else:
            print('  inexact pauses: 0')
        if exact:
            if verdict == 'PASS':
                verdict = 'WARN'
            print(f'  exact pauses (replayed, ownership lost 600 frames each): {len(exact)}')
            for (who, why), n in sorted(Counter((p[1], p[2]) for p in exact).items()):
                print(f'    {n} x {who} ({why})')
        else:
            print('  exact pauses: 0')
        if on['validation']:
            print(f"  validation: {on['validation_errors']} errors" + (' FAIL' if on['validation_errors'] else ''))
            if on['validation_errors']:
                verdict = 'FAIL'
        print(f'  -> {verdict}')
        if rank[verdict] > rank[worst]:
            worst = verdict
    if not any(r['on'] for r in runs):
        print('no run with owned steps on given')
        worst = 'FAIL'
    print(f'OWNED VERDICT {worst}')
    return 1 if worst == 'FAIL' else 0


if __name__ == '__main__':
    sys.exit(main())
