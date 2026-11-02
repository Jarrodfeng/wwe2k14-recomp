"""The owned-frame scoreboard (Session 82, docs\\native\\OWNED_FRAME_DESIGN.md sections 2 and 4): how much of the match
frame our renderer draws itself ("owned") and how much still goes through the emulated path, per group G1-G10 and in
total, weighted by what each group costs on the Surface. Every progress report on the owned frame leads with it.

    py -I tools\\native\\scoreboard.py RUN [--dir LOGDIR] [--frame F] [--label L] [--json] [--approx] [--check]
    py -I tools\\native\\scoreboard.py --make-baseline [--dir D] [--label L] RUN     (rewrites the ms in the baseline)

RUN is a run number (log wwe2k14_RUN.log in LOGDIR, default out\\build\\win-amd64-release\\logs) or a path to a log.
The groups, the rules that sort a pass into one and the Surface ms per group are in tools\\native\\
scoreboard_baseline.json (rules hand-written, ms from Surface run 040's per-pass medians).

Two inputs:
  1. The owned-frame census (the SDK's [native][owned-frame] frame F pass N: owned|fallback <reason> ... lines, section
     4 of the design): exact - each pass is owned or a fallback with its reason.
  2. Today's logs have no such lines: APPROXIMATE (no owned-frame census). A pass counts as owned when its group's
     part is in the run's gpu_native_own_steps list (the ring chain, the wrestler maps' casters and blurs; not G5's
     reductions) AND the run's 'owned steps per frame' census lines show draws going into our images. The truth
     passes that remain of an owned chain (its clear passes) count as owned.
Passes come from the [native][pass] lines (the pass log: --wwe2k14_pass_log / the passlog route command; the Surface
runs). Groups' Surface ms are shared out over their passes (a part's ms x owned passes / its passes).
--check: classify every logged frame and print every pass no rule sorts into a group (exit 1 if any).
"""
import argparse
import json
import re
import statistics
import sys
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LOGS = ROOT / 'out' / 'build' / 'win-amd64-release' / 'logs'
BASELINE = Path(__file__).with_name('scoreboard_baseline.json')

PASS_RE = re.compile(r'\[native\]\[pass\] frame (\d+) #(\d+) kind (\d+) (\d+)x(\d+) pitch (\d+) colour kind (\d+) '
                     r'depth (\d) draws (\d+) vs ([0-9A-F]+) ps ([0-9A-F]+): ([0-9.]+) ms \(before it ([0-9.]+) ms\)')
LABEL_RE = re.compile(r'passlog: (\S+)')
STEPS_RE = re.compile(r"(?:modified: gpu_native_own_steps = '([^']*)'|cvar gpu_native_own_steps = (\S+) \(frame)")
CENSUS_RE = re.compile(r'owned steps per frame: draws into our images ([\d.]+)')
# Section 4 of the design: one line per pass.
FRAME_RE = re.compile(r'\[native\]\[owned-frame\] frame (\d+) pass (\d+): (owned|fallback)(?: (.*?))? start (\S+) '
                      r'surface (\S+?)/(\d+)/(\S+) box (\d+)x(\d+) draws (\d+) vs ([0-9A-Fa-f]+) ps ([0-9A-Fa-f]+)'
                      r' copies (\d+)(?: \(served (\d+), page (\d+), dead (\d+), written back (\d+)\))?')
SUMMARY_RE = re.compile(r'\[native\]\[owned-frame\] frames (\d+)-(\d+): passes (\d+) owned (\d+) fallback (\d+)'
                        r'(?: \(by reason: ([^)]*)\))?(.*)')


def resolve_log(run, dirpath):
    if Path(run).exists():
        return Path(run)
    d = Path(dirpath)
    cands = [d / f'wwe2k14_{run}.log']
    if str(run).isdigit():
        cands += [d / f'wwe2k14_{int(run):03d}.log', d / f'wwe2k14_{int(run):04d}.log']
    for c in cands:
        if c.exists():
            return c
    sys.exit(f'no log {cands[0]} (also tried zero-padded names)')


def load_baseline():
    return json.loads(BASELINE.read_text(encoding='utf-8'))


def read_log(path):
    """Everything the scoreboard needs from one log, in one pass over it."""
    d = {'passes': defaultdict(dict), 'owned': defaultdict(dict), 'summaries': [], 'census': [], 'steps': None,
         'steps_src': '', 'frame_steps': {}, 'labels': {}}
    label = ''
    steps = None
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            if 'passlog:' in line:
                m = LABEL_RE.search(line)
                if m:
                    label = m.group(1)
                continue
            if 'gpu_native_own_steps' in line:
                m = STEPS_RE.search(line)
                if m:
                    steps = m.group(1) if m.group(1) is not None else m.group(2)
                    d['steps_src'] = 'log (modified: line)' if m.group(1) is not None else 'log (cvar line)'
                continue
            if '[native][pass]' in line:
                m = PASS_RE.search(line)
                if m:
                    fr, idx = int(m.group(1)), int(m.group(2))
                    d['passes'][(label, fr)][idx] = {
                        'idx': idx, 'w': int(m.group(4)), 'h': int(m.group(5)), 'pitch': int(m.group(6)),
                        'ck': int(m.group(7)), 'depth': int(m.group(8)), 'draws': int(m.group(9)),
                        'vs': m.group(10), 'ps': m.group(11), 'own_ms': float(m.group(12)),
                        'gap_ms': float(m.group(13))}
                    d['frame_steps'][(label, fr)] = steps
                continue
            if '[native][owned-frame]' in line:
                m = FRAME_RE.search(line)
                if m:
                    fr, idx = int(m.group(1)), int(m.group(2))
                    d['owned'][fr][idx] = {
                        'idx': idx, 'status': m.group(3), 'reason': (m.group(4) or '').strip(), 'start': m.group(5),
                        'pitch': int(m.group(7)), 'fmt': m.group(8), 'w': int(m.group(9)), 'h': int(m.group(10)),
                        'draws': int(m.group(11)), 'vs': m.group(12).upper(), 'ps': m.group(13).upper(),
                        'copies': int(m.group(14)), 'own_ms': 0.0, 'gap_ms': 0.0}
                    continue
                m = SUMMARY_RE.search(line)
                if m:
                    d['summaries'].append(m.groups())
                continue
            if 'owned steps per frame' in line:
                m = CENSUS_RE.search(line)
                if m:
                    d['census'].append(float(m.group(1)))
    d['steps'] = steps
    return d


def default_steps():
    """The arc defaults (src\\native_frame.h) when the log has no gpu_native_own_steps line."""
    text = (ROOT / 'src' / 'native_frame.h').read_text(encoding='utf-8', errors='replace')
    m = re.search(r'\{"gpu_native_own_steps",\s*((?:"[^"]*"\s*)+)\}', text)
    return ''.join(re.findall(r'"([^"]*)"', m.group(1))) if m else ''


# ---- classification -------------------------------------------------------------------------------------------

def rule_ok(rule, p, cur_gid, seen_ps):
    if 'index' in rule and p['idx'] != rule['index']:
        return False
    if 'pitch' in rule and p['pitch'] != rule['pitch']:
        return False
    if 'pitch_max' in rule and p['pitch'] > rule['pitch_max']:
        return False
    if 'ps' in rule and not p['ps'].startswith(rule['ps']):
        return False
    if 'vs' in rule and not p['vs'].startswith(rule['vs']):
        return False
    if 'from' in rule and cur_gid not in rule['from']:
        return False
    if 'ps_seen' in rule and not all(s in seen_ps for s in rule['ps_seen']):
        return False
    return True


def signature_group(p):
    """Census mode (Session 82): the owned frame drops dead passes (G2's impostors, G3, G4, G7 can be absent from its pass
    list), so the strict G1 -> G10 sequence of the start rules cannot sort it. A pass is sorted by what it IS instead: its box,
    pitch and first pixel shader (design doc section 2). None = no signature fits (then the sequence rules decide)."""
    ps, w, h, pitch = p['ps'][:8], p['w'], p['h'], p['pitch']
    if ps == '00000000' and pitch == 16:
        return 'G10'
    if ps in ('A7AD68BE', 'A5C9AC54', '1233F68C') or (pitch == 4 and (w, h) == (320, 180)):
        return 'G9'
    if (w, h) == (1280, 720) and pitch == 16 and p['draws'] >= 50:
        return 'G8'
    if ps == '8479D8DA':
        return 'G7'
    if ps == 'F235532B':
        return 'G4'
    if ps == '7ABDFEFB':
        return 'G3'
    if (w, h) == (64, 64):
        return 'G2'
    if (w, h) in ((128, 128), (32, 32)):
        return 'G5'
    if (w, h) in ((512, 256), (128, 64), (32, 16)):
        return 'G6'
    if (w, h) == (256, 256) or p['idx'] == 0:
        return 'G1'
    if (w, h) == (512, 512) and pitch == 7:
        return 'G5'
    return None


def part_of(g, p, seen):
    part = len(g['parts']) - 1
    for pi, prt in enumerate(g['parts']):
        if 'match' in prt and rule_ok(prt['match'], p, g['id'], seen):
            return pi
        if 'match' not in prt:
            return pi
    return part


def classify_frame(passes, groups, by_signature=False):
    """passes: {idx: pass}. Returns [(pass, group or None, part index)] in pass order. by_signature: census mode (see
    signature_group); a pass no signature fits falls back to the sequence rules."""
    cur = -1
    seen = set()
    out = []
    ids = {g['id']: g for g in groups}
    for idx in sorted(passes):
        p = passes[idx]
        sig = signature_group(p) if by_signature else None
        if sig and sig in ids:
            cur = max(cur, [g['id'] for g in groups].index(sig))   # (review fix: later unsigned passes continue from here)
            out.append((p, ids[sig], part_of(ids[sig], p, seen)))
            seen.add(p['ps'][:8])
            continue
        for gi in range(cur + 1, len(groups)):
            if rule_ok(groups[gi]['start'], p, groups[cur]['id'] if cur >= 0 else None, seen):
                cur = gi
                break
        if cur < 0:
            out.append((p, None, 0))
        else:
            out.append((p, groups[cur], part_of(groups[cur], p, seen)))
        seen.add(p['ps'][:8])
    return out


def pick_frame(frames, want):
    """frames: {key: {idx: pass}}. want: a frame number or None -> the last frame with the commonest pass count."""
    if want is not None:
        keys = [k for k in frames if k[1] == want]
        if not keys:
            sys.exit(f'frame {want}: no passes logged for it (frames: {sorted({k[1] for k in frames})[:12]} ...)')
        return keys[-1]
    counts = Counter(len(v) for v in frames.values())
    n = max(counts, key=lambda c: (counts[c], c))
    return [k for k, v in frames.items() if len(v) == n][-1]


# ---- the scoreboard -------------------------------------------------------------------------------------------

def step_owned(part, steps_text):
    need = part.get('owned_if_steps')
    if not need:
        return False
    have = [s.strip().upper() for s in steps_text.split(',') if s.strip()]
    return all(any(h.startswith(n.upper()) for h in have) for n in need)


def build_board(base, rows, approx, steps_text, census_on):
    groups = base['groups']
    board = []
    for g in groups:
        parts = []
        for prt in g['parts']:
            parts.append({'name': prt['name'], 'ms': prt['ms'], 'passes': 0, 'owned': 0, 'fallback': 0,
                          'reasons': Counter()})
        board.append({'id': g['id'], 'name': g['name'], 'parts': parts})
    unclassified = []
    for p, g, part in rows:
        if g is None:
            unclassified.append(p)
            continue
        b = next(x for x in board if x['id'] == g['id'])['parts'][part]
        b['passes'] += 1
        if approx:
            own = census_on and step_owned(g['parts'][part], steps_text)
            reason = ''
        else:
            own = p['status'] == 'owned'
            reason = p['reason']
        if own:
            b['owned'] += 1
        else:
            b['fallback'] += 1
            if reason:
                b['reasons'][reason.split(':')[0] if reason.startswith('other:') else reason] += 1
    out = []
    tot = {'passes': 0, 'owned': 0, 'ms': 0.0, 'ms_owned': 0.0}
    for b in board:
        passes = sum(x['passes'] for x in b['parts'])
        owned = sum(x['owned'] for x in b['parts'])
        ms = sum(x['ms'] for x in b['parts'] if x['passes'])
        ms_owned = sum(x['ms'] * x['owned'] / x['passes'] for x in b['parts'] if x['passes'])
        reasons = Counter()
        for x in b['parts']:
            reasons.update(x['reasons'])
        out.append({'id': b['id'], 'name': b['name'], 'passes': passes, 'owned': owned, 'ms': ms,
                    'ms_owned': ms_owned, 'reasons': dict(reasons),
                    'parts': [{'name': x['name'], 'passes': x['passes'], 'owned': x['owned'], 'ms': x['ms']}
                              for x in b['parts']]})
        tot['passes'] += passes
        tot['owned'] += owned
        tot['ms'] += ms
        tot['ms_owned'] += ms_owned
    return out, tot, unclassified


def print_board(title, mode, groups, tot, unclassified, extra):
    print(f'{title} - {mode}')
    for g in groups:
        if not g['passes']:
            continue
        line = (f"  {g['id']:<3} {g['name']:<14} {g['owned']:>2}/{g['passes']:<2} passes owned   "
                f"{g['ms_owned']:5.2f}/{g['ms']:5.2f} ms")
        if g['reasons']:
            line += '   fallback: ' + ', '.join(f'{k} {v}' for k, v in sorted(g['reasons'].items()))
        elif len(g['parts']) > 1 and 0 < g['owned'] < g['passes']:
            line += '   (' + ', '.join(f"{p['name']} {p['owned']}/{p['passes']}" for p in g['parts'] if p['passes']) + ')'
        print(line)
    pct = 100.0 * tot['ms_owned'] / tot['ms'] if tot['ms'] else 0.0
    print(f"owned {tot['owned']} of {tot['passes']} passes, {tot['ms_owned']:.1f} of {tot['ms']:.1f} Surface ms "
          f"({pct:.0f}%)")
    for line in extra:
        print(line)
    if unclassified:
        print(f'  UNCLASSIFIED passes ({len(unclassified)}):')
        for p in unclassified:
            print(f"    #{p['idx']} {p['w']}x{p['h']} pitch {p['pitch']} draws {p['draws']} vs {p['vs'][:8]} ps {p['ps'][:8]}")


def make_baseline(args, base):
    path = resolve_log(args.run, args.dir)
    d = read_log(path)
    label = args.label or base['source']['label']
    frames = {k: v for k, v in d['passes'].items() if k[0] == label}
    if not frames:
        sys.exit(f'no pass lines for label {label} in {path}')
    counts = Counter(len(v) for v in frames.values())
    n = max(counts, key=lambda c: (counts[c], c))
    use = [v for v in frames.values() if len(v) == n]
    groups = base['groups']
    rows0 = classify_frame(use[0], groups)
    if any(g is None for _, g, _ in rows0):
        sys.exit('the rules leave a pass unclassified in the first frame; fix the rules first')
    ms = defaultdict(float)
    for i in range(n):
        own = statistics.median(fr[i]['own_ms'] for fr in use)
        gap = statistics.median(fr[i]['gap_ms'] for fr in use)
        _, g, part = rows0[i]
        ms[(g['id'], part)] += own + gap
    for g in groups:
        for pi, prt in enumerate(g['parts']):
            prt['ms'] = round(ms[(g['id'], pi)], 3)
    totals = [sum(p['own_ms'] + p['gap_ms'] for p in fr.values()) for fr in use]
    keys = sorted(k[1] for k, v in frames.items() if len(v) == n)
    base['source'] = {
        'run': args.run if not str(args.run).isdigit() else int(args.run),
        'log': (path.resolve().relative_to(ROOT).as_posix() if ROOT in path.resolve().parents else str(path)),
        'label': label,
        'frames': keys,
        'passes_per_frame': n,
        'method': ('ms per pass = median over the frames of (own time + the gap before it); a group part sums its '
                   'passes (the design doc section 2 table: the same per-pass medians). Frames used: those with '
                   f'the commonest pass count ({n}).'),
        'frame_total_median_ms': round(statistics.median(totals), 3),
        'sum_of_group_ms': round(sum(ms.values()), 3),
        'note': ('Surface run 040, label match_drop: the match with the ring chain owned (steps 9C85DA5F, E1AD4BB4; '
                 'clears dropped), so the ms of G1 is its cost owned and G2-G10 are the 360 truth structure. This '
                 'run is before the wrestler maps (step 85) were owned, so G5 is the truth cost. G7 has no pass in '
                 'this run (PC-only pass, 0 ms). G2 has 20 passes here (15 on the PC), G5 8 (6 on the PC: the '
                 'owned blurs are not truth passes there); the ms of a part is shared over its passes.'),
    }
    BASELINE.write_text(json.dumps(base, indent=2) + '\n', encoding='utf-8')
    print(f'baseline written: {len(use)} frames x {n} passes, sum of groups {sum(ms.values()):.3f} ms, '
          f'frame median {statistics.median(totals):.3f} ms')
    for g in groups:
        print(f"  {g['id']:<3} " + '  '.join(f"{p['name']} {p['ms']:.3f}" for p in g['parts']))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('run')
    ap.add_argument('--dir', default=str(LOGS))
    ap.add_argument('--frame', type=int)
    ap.add_argument('--label', default='')
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--approx', action='store_true', help='ignore owned-frame census lines (today\'s mode)')
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--make-baseline', action='store_true')
    args = ap.parse_args()
    base = load_baseline()
    if args.make_baseline:
        make_baseline(args, base)
        return 0
    path = resolve_log(args.run, args.dir)
    d = read_log(path)
    groups = base['groups']
    use_census = bool(d['owned']) and not args.approx
    if use_census:
        frames = {('', fr): v for fr, v in d['owned'].items()}
    else:
        frames = {k: v for k, v in d['passes'].items() if not args.label or k[0] == args.label}
    if not frames:
        sys.exit(f'{path.name}: no ' + ('[native][owned-frame]' if use_census else '[native][pass]') +
                 ' lines' + (f' for label {args.label}' if args.label else '') +
                 ' - run with the pass log (segment/pass logging) on')
    unclassified_all = []
    if args.check:
        total = 0
        for key, v in sorted(frames.items()):
            for p, g, _ in classify_frame(v, groups, use_census):
                total += 1
                if g is None:
                    unclassified_all.append((key, p))
        print(f'{path.name}: classified {total - len(unclassified_all)} of {total} passes in {len(frames)} frames '
              f'({"owned-frame census" if use_census else "pass log"})')
        for key, p in unclassified_all:
            print(f"  UNCLASSIFIED frame {key[1]} #{p['idx']} {p['w']}x{p['h']} pitch {p['pitch']} draws {p['draws']} "
                  f"vs {p['vs'][:8]} ps {p['ps'][:8]}")
        return 1 if unclassified_all else 0
    key = pick_frame(frames, args.frame)
    rows = classify_frame(frames[key], groups, use_census)
    extra = []
    if use_census:
        steps_text, census_on = '', True
        mode = 'owned-frame census'
        for s in d['summaries'][-1:]:
            extra.append(f'  last 600-frame summary: frames {s[0]}-{s[1]}: passes {s[2]} owned {s[3]} fallback {s[4]}'
                         + (f' (by reason: {s[5]})' if s[5] else ''))
    else:
        steps_text = d['frame_steps'].get(key) or d['steps']
        src = d['steps_src']
        if not steps_text:
            steps_text, src = default_steps(), 'src\\native_frame.h arc defaults'
        census_on = (not d['census']) or any(c > 0 for c in d['census'])
        mode = 'approximate (no owned-frame census)'
        on_txt = ('on: ' + f"{sum(1 for c in d['census'] if c > 0)} of {len(d['census'])} 'owned steps per frame' "
                  'census lines have draws into our images' if d['census'] else 'no owned-steps census lines in the log')
        extra.append(f'  owned = group part is in gpu_native_own_steps ({src}) and the steps are on [{on_txt}]')
    groups_out, tot, unclassified = build_board(base, rows, not use_census, steps_text, census_on)
    title = f"run {path.stem.split('_')[-1]} frame {key[1]}" + (f" label {key[0]}" if key[0] else '') + \
            f" ({len(rows)} passes)"
    if args.json:
        print(json.dumps({'run': path.stem.split('_')[-1], 'frame': key[1], 'label': key[0], 'mode': mode,
                          'groups': groups_out, 'total': {
                              'passes': tot['passes'], 'owned_passes': tot['owned'],
                              'surface_ms': round(tot['ms'], 3), 'owned_surface_ms': round(tot['ms_owned'], 3),
                              'owned_ms_share': round(tot['ms_owned'] / tot['ms'], 4) if tot['ms'] else 0},
                          'unclassified': [{k: p[k] for k in ('idx', 'w', 'h', 'pitch', 'draws', 'vs', 'ps')}
                                           for p in unclassified]}, indent=1))
    else:
        print_board(title, mode, groups_out, tot, unclassified, extra)
    return 1 if unclassified else 0


if __name__ == '__main__':
    sys.exit(main())
