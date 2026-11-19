#!/usr/bin/env python3
"""S102 lane M3: the budget board. Generates docs\\renderer\\BUDGET_BOARD.md from the ledger, the lanes' claims and measurements.

  py -I tools\\board\\budget.py                 print the board (markdown) to stdout
  py -I tools\\board\\budget.py --write         write docs\\renderer\\BUDGET_BOARD.md (generated; never edit by hand)
  py -I tools\\board\\budget.py --check         parse every input; exit 1 when one cannot be read (a gate check)
  py -I tools\\board\\budget.py --selftest      planted-fault selftest (tools\\board\\budget_selftest.py)
  options: --ledger CSV --lanes DIR --lab CSV --trains CSV --out FILE (defaults below)

Per scene (match, brock = Brock's entrance, bryan = Bryan's entrance) and per side against the 16.7 ms frame of 60 fps:
  gpu     GPU work per frame (ms)            render  our render thread per frame (ms)   swap  the game's swap-to-swap time (ms)
  fps     shown frames per second            low1    shown 1 % low (fps)
Rows: baseline and target (hand-entered in the ledger), the newest full bench (ledger rows of kind 'bench', appended by
tools\\surface\\bench_line.py --ledger), and one row per lane: its CLAIM (the lane's gate json, top-level "claim": {"scene","side","ms":[lo,hi]},
or a list of them) and its MEASUREMENT (the Surface lab's results.csv). A lane with no measurement says "not measured": a lane without a
measured row is not done. Then the merge trains' waiting time per stage (out\\s102\\trains.csv, F1) and lanes running / waiting.

Ledger docs\\renderer\\BUDGET_LEDGER.csv (source of truth for hand-entered rows; the board is generated from it and the inputs):
  scene,side,kind,lane,lo,hi,unit,source,when,note
  scene  match|brock|bryan      side  gpu|render|swap|fps|low1       kind  baseline|target|bench|measured
  lane   the lane name (blank for baseline / target / bench)     lo,hi  numbers (equal for a single value)     unit  ms|fps
  source where the number came from (a bench dir, a run id)      when   'YYYY-MM-DD HH:MM' local time          note  free
A 'measured' ledger row stands in for a lab row (a count or a bench the lane's author read by hand); lab rows win when both exist.
Lab results out\\s102\\lab\\results.csv (written by tools\\surface\\lab.py, lane M2, one row per job: job_id,finished,capture,switches,status,...,
render_ms_med,gpu_ms_med,...). A claim names the switch it turns on: "claim": {"scene":"bryan","side":"gpu","ms":[-3,-1],"switch":"wwe2k14_rdr_x"}.
The measurement is the newest status-ok job on a capture of that scene (the capture name holds bryan / brock / match) whose switches include the
claim's switch, minus the newest status-ok job on the same capture without it (gpu = gpu_ms_med, render = render_ms_med; swap / fps / low1 are only
measured by full benches, ledger kind 'bench'). A long form  lane,scene,side,ms[,when]  is also read (hand-written rows, fixtures). A claim with no
switch or no pair of jobs reads "not measured".
Trains out\\s102\\trains.csv (F1): long form  lane,stage,when  (one row per lane per stage) or wide form  lane,<stage>,<stage>...
with stages in STAGES order; 'when' is 'YYYY-MM-DD HH:MM[:SS]'.
"""
import argparse, csv, json, re, subprocess, sys
from datetime import datetime
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]

SCENES = ('match', 'brock', 'bryan')
SCENE_TITLE = {'match': 'Match', 'brock': "Brock's entrance", 'bryan': "Bryan's entrance"}
SIDES = ('gpu', 'render', 'swap', 'fps', 'low1')
SIDE_TITLE = {'gpu': 'GPU ms', 'render': 'render thread ms', 'swap': 'swap thread ms', 'fps': 'shown fps', 'low1': '1% low fps'}
UNIT = {'gpu': 'ms', 'render': 'ms', 'swap': 'ms', 'fps': 'fps', 'low1': 'fps', 'note': '-'}
# S103 GRF: the ledger grew sentence rows (kind note / correction / dropped / verified / parked / passed / fidelity / relabel) and counts on other sides (device, frame, update, check, bench,
# scene `all`). The board's columns and sums read only SIDES and the kinds they name; a row outside them is checked for shape (scene, side, kind, lo <= hi) and for the unit only when its side is a board side.
EXTRA_SIDES = ('note', 'device', 'frame', 'update', 'check', 'bench')
LEDGER_SIDES = SIDES + EXTRA_SIDES
LEDGER_SCENES = SCENES + ('all',)
KINDS = ('baseline', 'target', 'bench', 'measured', 'reference', 'knockout', 'stamps', 'note', 'correction', 'dropped', 'verified', 'parked', 'passed', 'fidelity', 'relabel')   # S102 main: the last three are notes (STANDARD reference, S0-5 knock-out ceilings, GPU by role); the board's sums ignore them
LEDGER_COLS = ['scene', 'side', 'kind', 'lane', 'lo', 'hi', 'unit', 'source', 'when', 'note']
LAB_COLS = ['lane', 'scene', 'side', 'ms']
STAGES = ['brief', 'started', 'handback', 'review_started', 'review_done', 'landed_track', 'landed_session']
FRAME_MS = 16.7


class BadInput(Exception):
    pass


def main_checkout():
    try:
        common = subprocess.run(['git', '-C', str(REPO), 'rev-parse', '--path-format=absolute', '--git-common-dir'],
                                capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return Path(common).parent
    except Exception:
        pass
    return REPO


def num(s, where):
    try:
        return float(s)
    except (TypeError, ValueError):
        raise BadInput(f'{where}: {s!r} is not a number')


def read_ledger(path):
    p = Path(path)
    if not p.is_file():
        raise BadInput(f'ledger {p} does not exist')
    with open(p, newline='', encoding='utf-8') as f:
        rd = csv.DictReader(f)
        if rd.fieldnames != LEDGER_COLS:
            raise BadInput(f'ledger {p.name}: header is {rd.fieldnames}, expected {LEDGER_COLS}')
        rows = []
        for i, r in enumerate(rd, 2):
            if not any(r.values()):
                continue
            w = f'ledger {p.name} line {i}'
            if None in r or None in r.values():
                raise BadInput(f'{w}: wrong number of fields (a comma inside a text field? use ;)')
            if r['scene'] not in LEDGER_SCENES:
                raise BadInput(f"{w}: scene {r['scene']!r} not in {LEDGER_SCENES}")
            if r['side'] not in LEDGER_SIDES:
                raise BadInput(f"{w}: side {r['side']!r} not in {LEDGER_SIDES}")
            if r['kind'] not in KINDS:
                raise BadInput(f"{w}: kind {r['kind']!r} not in {KINDS}")
            if r['side'] in UNIT and r['unit'] != UNIT[r['side']]:
                raise BadInput(f"{w}: unit {r['unit']!r} for side {r['side']} must be {UNIT[r['side']]}")
            if r['kind'] == 'measured' and not r['lane']:
                raise BadInput(f'{w}: a measured row names its lane')
            r['lo'], r['hi'] = num(r['lo'], w), num(r['hi'], w)
            if r['lo'] > r['hi']:
                raise BadInput(f'{w}: lo {r["lo"]} > hi {r["hi"]}')
            rows.append(r)
    return rows


def read_claims(lanes_dir):
    """{lane: [claim dicts]} from each lane json's top-level "claim" (a dict or a list of dicts). A malformed claim is an error."""
    out = {}
    d = Path(lanes_dir)
    if not d.is_dir():
        raise BadInput(f'lanes folder {d} does not exist')
    for f in sorted(d.glob('*.json')):
        try:
            data = json.loads(f.read_text(encoding='utf-8'))
        except Exception as e:
            raise BadInput(f'{f.name}: unreadable lane json ({e})')
        c = data.get('claim')
        if c is None:
            continue
        for k in (c if isinstance(c, list) else [c]):
            w = f'{f.name} claim'
            if isinstance(k, dict) and list(k.get('ms') or []) in ([0, 0], [0.0, 0.0]):
                continue   # an instrument lane's "no saving" claim, or a lever not built: nothing to measure
            if isinstance(k, dict):   # S102 main: lanes write sides in words ("GPU", "our render thread", "game thread ..."): normalise
                k = dict(k)
                s = str(k.get('side', '')).lower()
                k['side'] = s if s in SIDES else ('gpu' if 'gpu' in s else 'render' if 'render' in s else 'swap' if ('swap' in s or 'game' in s) else s)
                k.setdefault('switch', k.get('lever', ''))
            # S103 GRF: a lane may name its scenes in words ("bryan and brock (the entrances ...)", "match and bryan, the D3 skips on"): one claim per scene word it names
            scenes = [k['scene']] if isinstance(k, dict) and k.get('scene') in SCENES else \
                [sc for sc in SCENES if isinstance(k, dict) and re.search(r'\b' + sc + r'\b', str(k.get('scene', '')).lower())]
            if not isinstance(k, dict) or not scenes or k.get('side') not in SIDES:
                raise BadInput(f'{w}: needs scene in {SCENES} and side in {SIDES}, got {k}')
            ms = k.get('ms')
            if not (isinstance(ms, list) and len(ms) == 2 and all(isinstance(x, (int, float)) for x in ms) and ms[0] <= ms[1]):
                raise BadInput(f'{w}: "ms" must be [lo, hi] numbers with lo <= hi, got {ms}')
            for sc in scenes:
                out.setdefault(f.stem, []).append({'scene': sc, 'side': k['side'], 'lo': float(ms[0]), 'hi': float(ms[1]), 'switch': k.get('switch', '')})
    return out


M2_COLS = ['job_id', 'finished', 'host', 'exe_sha', 'capture', 'switches', 'frames', 'captured_frames', 'warm_frames', 'repeat', 'status', 'drift_pct']
M2_SIDE_COL = {'gpu': 'gpu_ms_med', 'render': 'render_ms_med'}   # the lab times GPU and render thread; swap / fps / low1 come from full benches
SAME_FIELDS = ('exe_sha', 'host', 'frames', 'captured_frames', 'warm_frames', 'repeat')   # two jobs of one comparison must agree on all of these
DRIFT_MAX = 3.0        # |drift_pct| of both jobs must stay under this
MAX_AGE_H = 24.0       # the two jobs finished within this many hours of each other
CAPTURE_RE = re.compile(r'^(?:s\d+_)?(bryan|brock|match)(?:[_\d-]|$)')


def capture_scene(row):
    """The lab file's explicit `scene` column when it has one, else a FIXED prefix of the capture name: [sNNN_]bryan|brock|match followed by _ - a digit or
    the end (s100_match300, bryan_entrance). No substring search: 'brock_vs_bryan' is brock, 'xbryan' is nothing."""
    sc = (row.get('scene') or '').strip()
    if sc:
        return sc if sc in SCENES else None
    m = CAPTURE_RE.match(row['capture'].lower())
    return m.group(1) if m else None


def parse_switches(text):
    """'a=true b=0 c=flag d=' -> {'a': 'true', 'b': '0', 'c': 'flag', 'd': ''}. Separators: space ; ,"""
    out = {}
    for t in re.split(r'[;,\s]+', text or ''):
        t = t.strip().lstrip('-')
        if t:
            k, _, v = t.partition('=')
            out[k] = v
    return out


def is_on(v):
    """Only true / nonzero is on; a bare flag (empty value or 'flag') is on; false / 0 / no / off is off."""
    v = (v or '').strip().lower()
    if v in ('', 'flag', 'true', 'yes', 'on'):
        return True
    try:
        return float(v) != 0
    except ValueError:
        return False


def on_set(text):
    return {k: v for k, v in parse_switches(text).items() if is_on(v)}


def comparable(a, b):
    """-> None when the two job rows can be subtracted, else the reason text."""
    for k in SAME_FIELDS:
        if a.get(k, '') != b.get(k, ''):
            return f'{k} differs ({a.get(k, "")} vs {b.get(k, "")})'
    for r in (a, b):
        d = fnum(r.get('drift_pct'))
        if d is None:
            return f'drift_pct not recorded for job {r["job_id"]} (repeat {r.get("repeat")})'
        if abs(d) >= DRIFT_MAX:
            return f'drift_pct {d:+.1f} % of job {r["job_id"]} is not under {DRIFT_MAX:g} %'
    try:
        ta, tb = parse_when(a['finished']), parse_when(b['finished'])
    except BadInput as e:
        return str(e)
    if abs((ta - tb).total_seconds()) > MAX_AGE_H * 3600:
        return f'finished {abs((ta - tb).total_seconds()) / 3600:.0f} h apart (limit {MAX_AGE_H:g} h)'
    return None


def fnum(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


def read_lab(path, claims=None):
    """-> ({(lane, scene, side): (ms, when)}, path) ; ({}, None) when the file does not exist yet. A pair that cannot be compared gives (None, reason).
    Two shapes. (1) long form  lane,scene,side,ms[,when]  (hand-written, fixtures). (2) the lab's own file (tools\\surface\\lab.py, M2): one row per job.
    A lane's measurement there = the newest status-ok job on a capture of the claim's scene whose ON switches (true / nonzero / bare flag; k=false is OFF)
    include the claim's switch, minus a status-ok job on the same capture whose on-switches are exactly the first job's minus the claim's. The two
    must be comparable (SAME_FIELDS equal, |drift_pct| < 3 % both, finished within 24 h) else the cell reads "not comparable: <reason>"."""
    p = Path(path)
    if not p.is_file():
        return {}, None
    with open(p, newline='', encoding='utf-8') as f:
        rd = csv.DictReader(f)
        cols = rd.fieldnames or []
        rows = list(rd)
    got = {}
    if all(c in cols for c in LAB_COLS):
        for i, r in enumerate(rows, 2):
            w = f'lab results {p.name} line {i}'
            if r['scene'] not in SCENES or r['side'] not in SIDES:
                raise BadInput(f"{w}: scene {r['scene']!r} / side {r['side']!r} unknown")
            got[(r['lane'], r['scene'], r['side'])] = (num(r['ms'], w), r.get('when', ''))   # file order = time order: the last row wins
    elif all(c in cols for c in M2_COLS):
        ok_rows = [r for r in rows if r['status'] == 'ok']
        for col in M2_SIDE_COL.values():
            if ok_rows and col not in cols:
                raise BadInput(f'lab results {p.name}: has job rows but no {col} column (has {cols})')
        for r in ok_rows:
            for col in M2_SIDE_COL.values():
                if r[col] != '':
                    num(r[col], f'lab results {p.name} job {r["job_id"]} {col}')
        for lane, cl in (claims or {}).items():
            for c in cl:
                col = M2_SIDE_COL.get(c['side'])
                if not c.get('switch') or col is None:
                    continue
                want = {k for k in parse_switches(c['switch'])}
                mine = [r for r in ok_rows if capture_scene(r) == c['scene'] and want <= set(on_set(r['switches'])) and r[col] != '']
                if not mine:
                    continue
                m = mine[-1]
                need = {k: v for k, v in on_set(m['switches']).items() if k not in want}
                bases = [r for r in ok_rows if r['capture'] == m['capture'] and on_set(r['switches']) == need and r[col] != '']
                if not bases:
                    continue
                reasons = []
                for b in reversed(bases):
                    why = comparable(m, b)
                    if why is None:
                        got[(lane, c['scene'], c['side'])] = (float(m[col]) - float(b[col]), m['finished'])
                        break
                    reasons.append(why)
                else:
                    got[(lane, c['scene'], c['side'])] = (None, reasons[0])
    else:
        raise BadInput(f"lab results {p.name}: columns {cols} are neither lane,scene,side,ms nor the lab's job rows ({M2_COLS}...)")
    return got, p


def parse_when(s):
    for fmt in ('%Y-%m-%d %H:%M:%S', '%Y-%m-%d %H:%M'):
        try:
            return datetime.strptime(s.strip(), fmt)
        except ValueError:
            pass
    raise BadInput(f'time {s!r} is not YYYY-MM-DD HH:MM[:SS]')


def read_trains(path):
    """-> {lane: {stage: datetime}} or None when the file does not exist yet."""
    p = Path(path)
    if not p.is_file():
        return None
    with open(p, newline='', encoding='utf-8') as f:
        rd = csv.DictReader(f)
        cols = rd.fieldnames or []
        lanes = {}
        if cols[:3] == ['lane', 'stage', 'when']:
            for r in rd:
                if r['stage'] not in STAGES:
                    raise BadInput(f"trains {p.name}: stage {r['stage']!r} not in {STAGES}")
                lanes.setdefault(r['lane'], {})[r['stage']] = parse_when(r['when'])
        elif cols and cols[0] == 'lane':
            bad = [c for c in cols[1:] if c not in STAGES]
            if bad:
                raise BadInput(f'trains {p.name}: unknown stage columns {bad}')
            for r in rd:
                for st in cols[1:]:
                    if r[st]:
                        lanes.setdefault(r['lane'], {})[st] = parse_when(r[st])
        else:
            raise BadInput(f'trains {p.name}: header {cols} is neither lane,stage,when nor lane,<stage>...')
    return lanes


def waits(trains):
    """{(from_stage, to_stage): [minutes...]} between consecutive stages a lane has stamps for."""
    out = {}
    for lane, st in trains.items():
        have = [s for s in STAGES if s in st]
        for a, b in zip(have, have[1:]):
            out.setdefault((a, b), []).append((st[b] - st[a]).total_seconds() / 60.0)
    return out


def rng(lo, hi, unit):
    return f'{lo:.1f}' if abs(hi - lo) < 0.05 else f'{lo:.1f}..{hi:.1f}'


def render(ledger, claims, lab, lab_path, trains, when_note=''):
    L = ['# Budget board', '',
         '*Generated by `py -I tools\\board\\budget.py --write` from `docs\\renderer\\BUDGET_LEDGER.csv`, the lanes\' gate-json claims, the Surface lab '
         '(`out\\s102\\lab\\results.csv`) and the merge trains (`out\\s102\\trains.csv`). Never edit by hand.*', '',
         f'The frame to beat is {FRAME_MS} ms (60 fps). Sides: GPU work, our render thread, the game\'s swap thread (swap-to-swap), and what the '
         'player sees (shown fps, 1 % low). A lane row shows its claimed saving (ms, negative = faster) and its measurement; '
         '**a lane without a measured row is not done.**', '']
    if lab_path is None:
        L += ['> No Surface lab results yet (`out\\s102\\lab\\results.csv` does not exist): every lane reads "not measured".', '']
    unmeasured = []
    for sc in SCENES:
        L += [f'## {SCENE_TITLE[sc]}', '', '| row | ' + ' | '.join(SIDE_TITLE[s] for s in SIDES) + ' | source |',
              '|---|' + '---|' * (len(SIDES) + 1)]
        for kind, label in (('baseline', 'baseline'), ('target', 'target'), ('bench', 'newest full bench')):
            rows = [r for r in ledger if r['scene'] == sc and r['kind'] == kind]
            if kind == 'bench' and rows:   # the newest bench first (by when, then source), then its sides: never a mix of two benches
                newest = max((r['when'], r['source']) for r in rows)
                rows = [r for r in rows if (r['when'], r['source']) == newest]
            cells, src = [], ''
            for s in SIDES:
                rs = [r for r in rows if r['side'] == s]
                if rs:
                    r = rs[-1]
                    cells.append(rng(r['lo'], r['hi'], r['unit']))
                    src = r['source'] or src
                else:
                    cells.append('-')
            L.append(f'| {label} | ' + ' | '.join(cells) + f' | {src or "not entered"} |')
        for lane in sorted({l for l, cl in claims.items() for c in cl if c['scene'] == sc}):
            cells, srcs = [], []
            for s in SIDES:
                cs = [c for c in claims[lane] if c['scene'] == sc and c['side'] == s]
                if not cs:
                    cells.append('')
                    continue
                c = cs[0]
                m = lab.get((lane, sc, s))
                led = [r for r in ledger if r['kind'] == 'measured' and r['lane'] == lane and r['scene'] == sc and r['side'] == s]
                if m and m[0] is None:
                    meas = f'not comparable: {m[1]}'
                    unmeasured.append((lane, sc, s))
                elif m:
                    meas = f'**{m[0]:+.1f}** measured'
                    srcs.append('lab' + (f' {m[1]}' if m[1] else ''))
                elif led:
                    meas = f'**{led[-1]["lo"]:+.1f}** measured'
                    srcs.append(led[-1]['source'] or 'ledger')
                else:
                    meas = 'not measured'
                    unmeasured.append((lane, sc, s))
                cells.append(f'claim {rng(c["lo"], c["hi"], "ms")} / {meas}')
            L.append(f'| lane `{lane}` | ' + ' | '.join(x or '' for x in cells) + f' | {", ".join(srcs) or "claim only"} |')
        L.append('')
    L += ['## Lanes without a measured row (not done)', '']
    if unmeasured:
        L += [f'- `{l}`: {sc} / {SIDE_TITLE[s]}' for l, sc, s in sorted(set(unmeasured))]
    else:
        L += ['- none' if claims else '- no lane has a claim yet']
    L += ['', '## Merge trains: waiting time per stage', '']
    if trains is None:
        L += ['`out\\s102\\trains.csv` does not exist yet (F1 writes it): no waiting times.', '']
    else:
        w = waits(trains)
        L += ['| from | to | lanes | median min | max min |', '|---|---|---|---|---|']
        for (a, b), v in sorted(w.items(), key=lambda kv: (STAGES.index(kv[0][0]), STAGES.index(kv[0][1]))):
            v = sorted(v)
            L.append(f'| {a} | {b} | {len(v)} | {(v[len(v) // 2] + v[(len(v) - 1) // 2]) / 2:.0f} | {v[-1]:.0f} |')
        done = [l for l, st in trains.items() if 'landed_session' in st]
        running = [l for l, st in trains.items() if 'started' in st and 'handback' not in st]
        waiting = [l for l, st in trains.items() if 'handback' in st and 'landed_session' not in st]
        L += ['', f'Lanes running (started, no hand-back): {len(running)}'
              + (' (' + ', '.join(f'`{x}`' for x in sorted(running)) + ')' if running else ''),
              f'Lanes waiting (handed back, not landed on the session branch): {len(waiting)}'
              + (' (' + ', '.join(f'`{x}`' for x in sorted(waiting)) + ')' if waiting else ''),
              f'Lanes landed on the session branch: {len(done)}', '']
    return '\n'.join(L).rstrip() + '\n'


def build(args):
    main = main_checkout()
    root = Path(args.root)
    ledger = read_ledger(args.ledger or root / 'docs' / 'renderer' / 'BUDGET_LEDGER.csv')
    claims = read_claims(args.lanes or root / 'tools' / 'gate' / 'lanes')
    lab, lab_path = read_lab(args.lab or main / 'out' / 's102' / 'lab' / 'results.csv', claims)
    trains = read_trains(args.trains or main / 'out' / 's102' / 'trains.csv')
    return render(ledger, claims, lab, lab_path, trains)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=str(REPO))
    ap.add_argument('--ledger', default='')
    ap.add_argument('--lanes', default='')
    ap.add_argument('--lab', default='')
    ap.add_argument('--trains', default='')
    ap.add_argument('--out', default='')
    ap.add_argument('--write', action='store_true')
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        sys.path.insert(0, str(HERE))
        import budget_selftest
        return budget_selftest.run()
    try:
        text = build(a)
    except BadInput as e:
        print(f'FAIL  budget board: {e}')
        return 1
    if a.check:
        print(f'ok    budget board inputs parse ({len(text.splitlines())} lines)')
        return 0
    if a.write:
        out = Path(a.out) if a.out else Path(a.root) / 'docs' / 'renderer' / 'BUDGET_BOARD.md'
        out.write_text(text, encoding='utf-8', newline='\n')
        print(f'wrote {out}')
    else:
        sys.stdout.write(text)
    return 0


if __name__ == '__main__':
    sys.exit(main())
