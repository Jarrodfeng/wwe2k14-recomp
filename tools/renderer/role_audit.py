r"""S102 lane D1 (docs\renderer\DEVICE_LOST.md section 10): the synchronisation audit of a replayed capture. Offline, 0 runs.
  py -I tools\renderer\role_audit.py AUDIT.jsonl [--window A-B] [--selftest]
AUDIT.jsonl comes from rplay --role-audit FILE (one line per recorded step: the images a pass draws into / samples, a copy's source and destination, a
dispatch's cube, the present's source, each as [id, width, height, undefined-before-the-step]). Reports:
  plan       steps / passes / copies a frame, and the frames where the copy count changes by 5 or more (loss 488's plan changed 36 -> 16 copies 3 frames before it)
  new roles  (image, role) pairs first seen after the warm-up (frame 10), with the frame; those inside --window are flagged
  undefined  an image sampled, copied from or presented while every subresource is still UNDEFINED (never written by the GPU in this run)
  resized    an image id seen with two sizes
  feedback   a pass that samples one of its own targets; a copy whose source is its destination
Exit 1 when --selftest's planted lines are not all reported (the reader is blind) or the file cannot be read."""
import argparse, collections, json, sys


def load(path):
    rows = []
    with open(path, encoding='utf-8') as f:
        for n, line in enumerate(f, 1):
            line = line.strip()
            if line:
                rows.append(json.loads(line))
    return rows


def audit(rows, window=None, warm=10):
    out = collections.defaultdict(list)
    frames = collections.OrderedDict()
    first_role = {}
    sizes = collections.defaultdict(set)
    for r in rows:
        f = r['f']
        st = frames.setdefault(f, collections.Counter())
        st['steps'] += 1
        st[r['k']] += 1
        uses = []
        if r['k'] == 'pass':
            uses += [('target', i) for i in r['t']] + ([('depth', r['d'])] if r['d'][0] else []) + [('sampled', i) for i in r['r']]
            tids = {i[0] for i in r['t']} | ({r['d'][0]} if r['d'][0] else set())
            for i in r['r']:
                if i[0] in tids:
                    out['feedback'].append('frame %d step %d: pass %d samples its own target image %d' % (f, r['s'], r['i'], i[0]))
        elif r['k'] == 'copy':
            uses += [('copy_src', r['src']), ('copy_dst', r['dst'])]
            if r['src'][0] == r['dst'][0]:
                out['feedback'].append('frame %d step %d: copy %d from image %d into itself' % (f, r['s'], r['i'], r['src'][0]))
        elif r['k'] == 'dispatch':
            uses += [('dispatch', i) for i in r['t']]
        else:
            uses += [('present_src', r['src'])]
        for role, (iid, w, h, u) in uses:
            if not iid:
                continue
            sizes[iid].add((w, h))
            if u and role in ('sampled', 'copy_src', 'present_src'):
                out['undefined'].append('frame %d step %d: %s %d: image %d is %s while still UNDEFINED (%dx%d)' % (f, r['s'], r['k'], r['i'], iid, role, w, h))
            key = (iid, role)
            if key not in first_role:
                first_role[key] = (f, r['s'], r['k'], r['i'])
                if f >= warm:
                    inw = window and window[0] <= f <= window[1]
                    out['new_roles'].append('%sframe %d step %d: image %d (%dx%d) first used as %s (%s %d)' % ('IN WINDOW ' if inw else '', f, r['s'], iid, w, h, role, r['k'], r['i']))
    for iid, ss in sizes.items():
        if len(ss) > 1:
            out['resized'].append('image %d seen at %s' % (iid, sorted(ss)))
    prev = None
    for f, st in frames.items():
        if prev is not None and abs(st['copy'] - prev[1]['copy']) >= 5:
            out['plan'].append('frame %d -> %d: copies %d -> %d, steps %d -> %d, passes %d -> %d' % (prev[0], f, prev[1]['copy'], st['copy'], prev[1]['steps'], st['steps'], prev[1]['pass'], st['pass']))
        prev = (f, st)
    return out, frames


def selftest():
    good = [{'f': 0, 's': 0, 'm': 0, 'k': 'pass', 'i': 0, 't': [[5, 64, 64, 1]], 'd': [0, 0, 0, 0], 'load': 1, 'r': []}]
    planted = good + [
        {'f': 11, 's': 0, 'm': 0, 'k': 'pass', 'i': 0, 't': [[5, 64, 64, 0]], 'd': [0, 0, 0, 0], 'load': 0, 'r': [[5, 64, 64, 0]]},     # feedback + new role
        {'f': 12, 's': 0, 'm': 0, 'k': 'copy', 'i': 0, 'src': [7, 32, 32, 1], 'dst': [8, 32, 32, 1], 'rect': [0, 0, 32, 32]},              # undefined read
        {'f': 13, 's': 0, 'm': 0, 'k': 'pass', 'i': 0, 't': [[5, 128, 128, 0]], 'd': [0, 0, 0, 0], 'load': 1, 'r': []},                    # resized
    ] + [{'f': 14, 's': k, 'm': 0, 'k': 'copy', 'i': k, 'src': [9, 4, 4, 0], 'dst': [10, 4, 4, 0], 'rect': [0, 0, 4, 4]} for k in range(6)]  # plan change
    o_good, _ = audit(good)
    o, _ = audit(planted)
    ok = not any(o_good.values()) and all(o.get(k) for k in ('feedback', 'new_roles', 'undefined', 'resized', 'plan'))
    print(('PASS' if ok else 'FAIL') + ' selftest: a clean line reports nothing; planted feedback / new role / undefined read / resize / plan change are all reported (%s)' %
          {k: len(v) for k, v in o.items()})
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('audit', nargs='?')
    ap.add_argument('--window', default='')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    try:
        rows = load(a.audit)
    except Exception as e:
        print('FAIL cannot read %s: %s' % (a.audit, e))
        return 1
    if not rows:
        print('FAIL %s holds no step' % a.audit)
        return 1
    window = tuple(int(x) for x in a.window.split('-')) if a.window else None
    out, frames = audit(rows, window)
    print('%s: %d steps over %d frames (%d..%d)' % (a.audit, len(rows), len(frames), min(frames), max(frames)))
    for k in ('plan', 'feedback', 'undefined', 'resized', 'new_roles'):
        v = out.get(k, [])
        print('%s: %d' % (k, len(v)))
        for line in v[:40]:
            print('   ' + line)
        if len(v) > 40:
            print('   ... %d more' % (len(v) - 40))
    return 0


if __name__ == '__main__':
    sys.exit(main())
