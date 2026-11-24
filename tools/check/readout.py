"""S106 C2: read a run's log for a check recipe: join, window, aggregate, bars, arm verdict (docs/research/s106/C2.md section 2, steps 8-12).

A readout whose line never appears gives count 0, and for every other aggregate UNREAD (None): never a pass. Pure functions: the runner and
the selftest call the same ones. 0 game runs.
"""
# entry: evaluate
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import recipe as R  # noqa: E402

SHOT_RE = re.compile(r'screenshot saved: (.+?\.png) \((\d+)x(\d+)\)')


def read_log(log_path):
    """wwe2k14_NNN.log + rotated parts wwe2k14_NNN.K.log (highest K first = oldest first), whole, in Python. -> list of lines."""
    base = log_path[:-4]
    parts = []
    for p in glob.glob(glob.escape(base) + '.*.log'):
        m = re.search(r'\.(\d+)\.log$', p)
        if m:
            parts.append((int(m.group(1)), p))
    parts.sort(reverse=True)
    lines = []
    for _, p in parts + [(0, log_path)]:
        with open(p, encoding='utf-8', errors='replace') as f:
            lines += f.read().splitlines()
    return lines


def _window(lines, it):
    lo, hi = 0, len(lines)
    if 'after' in it:
        rx = re.compile(it['after'])
        for i, l in enumerate(lines):
            if rx.search(l):
                lo = i + 1
                break
        else:
            return None          # the marker never appeared: the window is empty and the readout UNREAD
    if 'before' in it:
        rx = re.compile(it['before'])
        for i in range(lo, len(lines)):
            if rx.search(lines[i]):
                hi = i
                break
    return lines[lo:hi]


def aggregate(lines, it):
    """-> int / frozenset / None (UNREAD)."""
    win = _window(lines, it)
    rx = re.compile(it['line'])
    agg = it['agg']
    vals = []
    n = 0
    for l in (win or []):
        m = rx.search(l)
        if m:
            n += 1
            if agg != 'count':
                try:
                    vals.append(int(m.group('v'), 0))
                except (IndexError, ValueError, TypeError):
                    return None
    if agg == 'count':
        return n
    if not vals:
        return None
    return {'max': max, 'min': min, 'sum': sum, 'last': lambda v: v[-1], 'first': lambda v: v[0], 'set': frozenset}[agg](vals)


def shots(lines, rec):
    """Screenshots the run saved: [(label, path)] picked by the recipe's labels (globs allowed)."""
    import fnmatch
    allshots = []
    for l in lines:
        m = SHOT_RE.search(l)
        if m:
            allshots.append(m.group(1))
    out = []
    for p in allshots:
        stem = os.path.splitext(os.path.basename(p))[0]
        label = re.sub(r'^\d+_', '', stem)
        for s in rec.get('shots', []):
            if any(fnmatch.fnmatch(label, g) for g in s['labels']):
                out.append((label, p))
                break
    return out


def evaluate(rec, lines):
    """-> dict: guards [{name, value, bar, ok}], readouts [{name, role, value, fault, fixed}], shots."""
    gs = []
    for g in R.all_guards(rec):
        v = aggregate(lines, g)
        gs.append(dict(name=g['name'], value=_j(v), bar=g['need'], ok=(R.meets(v, g['need']) if v is not None else False), unread=v is None))
    rs = []
    for it in rec.get('readout', []):
        v = aggregate(lines, it)
        rs.append(dict(name=it['name'], role=it['role'], value=_j(v), unread=v is None, fault=it.get('fault'), fixed=it['fixed'],
                       meets_fault=(R.meets(v, it['fault']) if v is not None and 'fault' in it else None),
                       meets_fixed=(R.meets(v, it['fixed']) if v is not None else None)))
    return dict(guards=gs, readouts=rs, shots=shots(lines, rec))


def _j(v):
    return sorted(v) if isinstance(v, frozenset) else v


def arm_verdict(rec, arm, ev, answers=None, tripwire_new=0, void=None):
    """arm = 'before' | 'after'. answers = {'yes'|'no'} for the shot question (EYES), or None. -> verdict text."""
    if void == 'abort':      # S106 C2F F13: run_list refused after the pending line: not a run
        return 'REFUSED'
    if void == 'alone':      # S106 C5F B1: run_list said the PC was not alone / not proven alone: the readout is not judged
        return 'BLOCKED-ALONE'
    if void:
        return 'VOID:' + void
    if any(not g['ok'] for g in ev['guards']):
        return 'VOID:guard'
    need = [r for r in ev['readouts'] if (arm == 'after' or r['role'] == 'fault')]
    if any(r['unread'] for r in need):
        return 'VOID:readout'
    # S106 C2F B2: recipe.eyes = "required": the shots decide both arms; a readout (a count the fault may share with other causes) never alone
    has_fault_readout = any(r['role'] == 'fault' for r in ev['readouts']) and rec.get('recipe', {}).get('eyes') != 'required'
    shots_cfg = rec.get('shots', [])
    want = (shots_cfg[0]['fault'] if arm == 'before' else shots_cfg[0]['fixed']) if shots_cfg else None
    if arm == 'before':
        if has_fault_readout:
            return 'FAULT' if all(r['meets_fault'] for r in ev['readouts'] if r['role'] == 'fault') else 'BEFORE-BLIND'
        if answers is None:
            return 'EYES'
        return 'FAULT' if answers == want else 'BEFORE-BLIND'
    ok = all(r['meets_fixed'] for r in ev['readouts'])
    if answers is not None and shots_cfg and answers != want:
        ok = False
    if not ok:
        return 'FAIL'
    if answers is None and shots_cfg and not has_fault_readout:
        return 'EYES'
    return 'FLAG' if tripwire_new else 'PASS'


def compare(before, after):
    """Verdicts of the two arms -> one combined verdict. BEFORE that does not show the fault is never a PASS."""
    if before is None or after is None:
        return 'PENDING'
    if before.startswith('VOID') or after.startswith('VOID'):
        return 'VOID'
    if before == 'BEFORE-BLIND':
        return 'BEFORE-BLIND'
    if before == 'EYES' or after == 'EYES':
        return 'EYES'
    if before != 'FAULT':
        return 'VOID'
    return after if after in ('PASS', 'FAIL', 'FLAG') else 'VOID'
