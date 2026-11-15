r"""S102 lane M1: selftest of tools\budget\budget_check.py on made-up counts (no rplay, no game data). Every "must fail" case is a planted fault.
  py -I tools\budget\budget_selftest.py
Includes the review's planted tests (pass keys shift when a pass is removed, an empty run, a frame without `pass`, a baseline from another capture).
"""
import copy, json, os, sys, tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import budget_check as bc


def call(**kw):
    c = {k: 0 for k in bc.CALL}
    c.update(kw)
    return c


def frame(i, passes):
    tot = call()
    for p in passes:
        for k in bc.CALL:
            tot[k] += p[k]
    return {'frame': i, 'dropped': 0, 'passes': len(passes), 'steps': 3, 'texture_sets': 2, 'ring_constants': 100, 'ring_geometry': 50, 'ring_prev': 0, 'upload_bytes': 0, 'total': tot, 'pass': passes}


def pas(i, **kw):
    p = call(**kw)
    p.update(index=i, width=64 + 16 * i, height=64, color_count=1, first_op=i * 10, target_image=10 + i, target_mip=0, target_layer=0, target_format=37)
    return p


def counts(n=4):
    return {'format': 'rdr-counts', 'version': 1, 'frames': [frame(i, [pas(0, draws=5, area_px=1000), pas(1, indexed_draws=7, indices=70, area_px=2000)]) for i in range(n)]}


def many(n=4, passes=8):
    """Eight distinct passes; pass 3 is a clear-only pass with no draws, the others draw."""
    out = []
    for i in range(n):
        ps = [pas(k, draws=0, clears=2) if k == 3 else pas(k, indexed_draws=k + 2, indices=10 * (k + 2), area_px=500 * (k + 1)) for k in range(passes)]
        out.append(frame(i, ps))
    return {'format': 'rdr-counts', 'version': 1, 'frames': out}


def remove_pass(d, which):
    """Remove pass `which` from every frame (an improvement) and renumber the later ones, as a merge of passes does."""
    e = copy.deepcopy(d)
    for f in e['frames']:
        gone = f['pass'].pop(which)
        for p in f['pass'][which:]:
            p['index'] -= 1
        for k in bc.CALL:
            f['total'][k] -= gone[k]
        f['passes'] -= 1
    return e


def check(name, ok, why=''):
    print('%s  %s %s' % ('PASS' if ok else 'FAIL', name, why))
    return ok


def run(cur_d, base_d, **kw):
    return bc.compare(bc.summarise(cur_d), bc.summarise(base_d), **kw)


def raises(fn):
    try:
        fn()
    except bc.Bad as e:
        return str(e)
    return None


def main():
    good = True
    base = counts()
    r, f = run(copy.deepcopy(base), base)
    good &= check('same counts pass', not r and not f)
    # planted fault: one extra draw in pass 1 of every frame
    bad = copy.deepcopy(base)
    for fr in bad['frames']:
        fr['pass'][1]['indexed_draws'] += 1
        fr['total']['indexed_draws'] += 1
    r, f = run(bad, base)
    good &= check('planted extra draw fails and names pass 1', bool(r) and any('(index 1,' in x for x in r) and not any('(index 0,' in x for x in r), '; '.join(r[:2]))
    bad = copy.deepcopy(base)
    bad['frames'][2]['pass'][0]['draws'] += 1
    bad['frames'][2]['total']['draws'] += 1
    r, f = run(bad, base)
    good &= check('one extra draw in one frame fails', bool(r))
    low = copy.deepcopy(base)
    low['frames'][0]['pass'][0]['draws'] -= 1
    low['frames'][0]['total']['draws'] -= 1
    r, f = run(low, base)
    good &= check('a lower count passes and is reported', not r and bool(f))
    bad = copy.deepcopy(base)
    bad['frames'][0]['pass'][0]['draws'] += 1
    bad['frames'][0]['total']['draws'] += 1
    r0, _ = run(bad, base)
    r1, _ = run(bad, base, tol_abs=1)
    good &= check('tolerance 0 fails, tolerance 1 passes', bool(r0) and not r1)
    bad = copy.deepcopy(base)
    for fr in bad['frames']:
        fr['pass'].append(pas(2, draws=1))
        fr['total']['draws'] += 1
        fr['passes'] += 1
    r, f = run(bad, base)
    good &= check('a new pass with work fails (the totals rise)', any('total sum draws' in x for x in r))
    good &= check('a different frame count cannot run', raises(lambda: run(counts(5), base)) is not None)
    bad = copy.deepcopy(base)
    for fr in bad['frames']:
        fr['dropped'] = 1
    r, f = run(bad, base)
    good &= check('a dropped frame (frame_ring_full) rises', any('dropped' in x for x in r))

    # review A: pass keys must not shift when a pass comes or goes
    mb = many()
    r, f = run(remove_pass(mb, 3), mb)
    good &= check('A: removing a pass (every later index shifts) passes, nothing rises', not r and bool(f), 'rises %d: %s' % (len(r), r[:2]))
    e = remove_pass(mb, 3)
    for fr in e['frames']:   # a real regression next to the removal (the removed pass drew nothing, so the totals show it)
        fr['pass'][5]['indexed_draws'] += 1
        fr['total']['indexed_draws'] += 1
    r, f = run(e, mb)
    good &= check('A: the same removal plus a real extra draw still fails (totals decide)', bool(r) and any('total sum indexed_draws' in x for x in r))
    e = copy.deepcopy(mb)
    for fr in e['frames']:
        fr['pass'][5]['indexed_draws'] += 1
        fr['total']['indexed_draws'] += 1
    r, f = run(e, mb)
    good &= check('A: a real +1 draw in one pass names that pass (not its neighbours)', any('(index 5,' in x for x in r) and not any('(index 4,' in x or '(index 6,' in x for x in r), '; '.join(r[:2]))
    # the same fallback works for a counts file without target fields (shape keys)
    old = copy.deepcopy(mb)
    for fr in old['frames']:
        for p in fr['pass']:
            for k in ('target_image', 'target_mip', 'target_layer', 'target_format'):
                del p[k]
    r, f = run(remove_pass(old, 3), old)
    good &= check('A: shape-keyed counts (no target fields) also survive a removal', not r)

    # review B: empty runs are not passes
    empty = copy.deepcopy(base)
    for fr in empty['frames']:
        fr['pass'] = []
        fr['passes'] = 0
        fr['total'] = call()
    why = raises(lambda: run(empty, base))
    good &= check('B: an all-zero run with the right frame count cannot run', why is not None and 'empty' in why, why or '')
    half = copy.deepcopy(base)
    for fr in half['frames']:
        fr['pass'][1]['indexed_draws'] = 0
        fr['total']['indexed_draws'] = 0
    half2 = copy.deepcopy(half)
    for fr in half2['frames']:
        fr['pass'][0]['draws'] = 1
        fr['total']['draws'] = 1
    why = raises(lambda: run(half2, base))
    good &= check('B: draws under half the baseline cannot run', why is not None and 'under 50' in why, why or '')
    good &= check('B: --accept-falls allows a deliberate drop', raises(lambda: run(half2, base, accept_falls=True)) is None)

    # loud failures on unreadable input
    with tempfile.TemporaryDirectory() as d:
        broken = copy.deepcopy(base)
        del broken['frames'][1]['total']['draws']
        p = Path(d) / 'c.json'
        p.write_text(json.dumps(broken))
        good &= check('a missing field fails loudly', raises(lambda: bc.load_counts(p)) is not None)
        nopass = copy.deepcopy(base)
        del nopass['frames'][1]['pass']
        p2 = Path(d) / 'np.json'
        p2.write_text(json.dumps(nopass))
        good &= check('H: a frame without the pass key is a named error', 'pass' in (raises(lambda: bc.load_counts(p2)) or ''))
        good &= check('an absent counts file fails loudly', raises(lambda: bc.load_counts(Path(d) / 'nope.json')) is not None)
        (Path(d) / 'e.json').write_text(json.dumps({'format': 'rdr-counts', 'version': 1, 'frames': []}))
        good &= check('an empty counts file fails loudly', raises(lambda: bc.load_counts(Path(d) / 'e.json')) is not None)
        # the CLI end to end on files
        cp, bp = Path(d) / 'cur.json', Path(d) / 'base.json'
        cp.write_text(json.dumps(base))
        rc = bc.main(['--counts', str(cp), '--write-baseline', str(bp)])
        rc2 = bc.main(['--counts', str(cp), '--baseline', str(bp)])
        bad = copy.deepcopy(base)
        bad['frames'][0]['pass'][1]['indexed_draws'] += 1
        bad['frames'][0]['total']['indexed_draws'] += 1
        cp.write_text(json.dumps(bad))
        rc3 = bc.main(['--counts', str(cp), '--baseline', str(bp)])
        good &= check('cli: baseline written, same passes (0), planted fault fails (1)', rc == 0 and rc2 == 0 and rc3 == 1, 'rc %d %d %d' % (rc, rc2, rc3))
        good &= check('cli: a missing baseline cannot run (2)', bc.main(['--counts', str(cp), '--baseline', str(Path(d) / 'missing.json')]) == 2)
        cp.write_text(json.dumps(empty))
        good &= check('B: cli exit 2 for the all-zero run', bc.main(['--counts', str(cp), '--baseline', str(bp)]) == 2)
        cp.write_text(json.dumps(nopass))
        good &= check('H: cli exit 2 (not a KeyError) for a frame without pass', bc.main(['--counts', str(cp), '--baseline', str(bp)]) == 2)
        # H: the capture fingerprint refuses another capture / window
        cap = Path(d) / 'cap'
        (cap / 'frames').mkdir(parents=True)
        for n in ('images.bin', 'geometry.bin', 'textures.bin'):
            (cap / n).write_bytes(b'x' * 100)
        for i in range(5):
            (cap / 'frames' / ('frame_%06d.bin' % i)).write_bytes(b'y' * (50 + i))
        m1 = bc.capture_meta(cap, 3)
        bc.check_meta(m1, bc.capture_meta(cap, 3))
        good &= check('H: the same capture and window match', True)
        (cap / 'frames' / 'frame_000001.bin').write_bytes(b'y' * 999)
        good &= check('H: a changed frame file is refused', raises(lambda: bc.check_meta(m1, bc.capture_meta(cap, 3))) is not None)
        good &= check('H: another window is refused', raises(lambda: bc.check_meta(m1, bc.capture_meta(cap, 4))) is not None)
        good &= check('H: a baseline without a fingerprint is refused for a capture run', raises(lambda: bc.check_meta(None, m1)) is not None)
    # the committed baseline reads
    bp = Path(__file__).resolve().parent / 'bryan_entrance.json'
    good &= check('the committed Bryan baseline reads (version 2, fingerprint, passes)', raises(lambda: bc.load_baseline(bp)) is None and bool(json.loads(bp.read_text())['meta']))
    print('selftest: %s' % ('PASS' if good else 'FAIL'))
    return 0 if good else 1


if __name__ == '__main__':
    sys.exit(main())
