r"""S101 lane PN: are the post draws named at their source?

  py -I tools\names\post_names_check.py --static            offline: the q index's queue callers (22 sites in 13 functions, the direct callers of
                                                             82C1F420 / 4C0 / 610) all have a row in origin_table.h with a technique
  py -I tools\names\post_names_check.py CAPTURE [...]       the LIVE check's reader: a capture (case zip or folder) taken with a build that has
                                                             the lane PN hooks; every chain draw must be named by its effect function (not by a
                                                             generic queue site), per shape the names of tools\names\post_names_expect.json.
--synth (selftest only): rewrites each chain draw's call site in memory to its expected function, to prove the PASS path.
Exit 1 on any FAIL or an unreadable input. A capture from a build without the hooks FAILS (its chain draws sit on the generic queue sites): that is the
check's positive control (post_names_selftest.py runs it on the Check B zip). Numbers, addresses and effect names only.
"""
import collections, json, os, re, subprocess, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(REPO / 'tools' / 'post'))
EXPECT = json.loads((HERE / 'post_names_expect.json').read_text(encoding='utf-8'))
H = lambda s: int(s, 16)
fails = []


def report(ok, name, detail=''):
    print('%s  %-34s %s' % ('PASS' if ok else 'FAIL', name, detail))
    if not ok:
        fails.append(name)


def table_rows():
    txt = (REPO / 'src' / 'renderer_adapter' / 'origin_table.h').read_text(encoding='utf-8')
    sub = txt.split('kSubmitters[]', 1)[1]
    rows = []
    for m in re.finditer(r'\{0x([0-9A-F]{8})u, 0x([0-9A-F]{8})u, DrawClass::(\w+), "([^"]*)"\}', sub):
        rows.append((int(m.group(1), 16), int(m.group(2), 16), m.group(3), m.group(4)))
    if not rows:
        raise SystemExit('post_names_check: no submitter rows read from origin_table.h')
    return rows


def row_of(rows, site):
    return next((r for r in rows if r[0] <= site < r[1]), None)


def q_callers(addr):
    p = subprocess.run([sys.executable, '-I', str(REPO / 'tools' / 'q' / 'q.py'), 'callers', addr], capture_output=True, text=True, cwd=REPO)
    out = {}
    for line in p.stdout.splitlines():
        m = re.match(r'\s+sub_([0-9A-F]{8})\s+(.*?)\s+\[ran', line)
        if m:
            out[int(m.group(1), 16)] = [int(x, 16) for x in re.findall(r'0x([0-9A-F]{8}) (?:call|tail)', m.group(2))]
    m = re.search(r'(\d+) call sites in (\d+) functions', p.stdout)
    return out, (int(m.group(1)), int(m.group(2))) if m else None


def static():
    rows = table_rows()
    c, tot = q_callers('0x82C1F5E8')
    sites = [s for v in c.values() for s in v]
    report(tot == (22, 13) and len(sites) == 22 and len(c) == 13, 'queue helper callers', 'q index: %s, parsed %d sites in %d functions' % (tot, len(sites), len(c)))
    missing = [hex(f) for f in c if not (row_of(rows, f) and row_of(rows, f)[3])]
    report(not missing, 'every effect function has a technique', 'unnamed: %s' % missing if missing else '%d functions' % len(c))
    exp = {H(x) for x in EXPECT['effect_functions']}
    report(exp <= set(rows_starts(rows)), 'expected effect set is in the table', '')
    report(set(c) <= exp, 'queue callers inside the expected effect set', 'extra: %s' % [hex(f) for f in set(c) - exp])
    for q in ('0x82C1F420', '0x82C1F4C0', '0x82C1F610'):
        cc, _ = q_callers(q)
        direct = [f for f in cc if f != 0x82C1F5E8]
        bad = [hex(f) for f in direct if not row_of(rows, f)]
        report(not bad, 'direct callers of %s have rows' % q[2:], 'unnamed: %s' % bad if bad else ', '.join(hex(f) for f in direct))


def rows_starts(rows):
    return [r[0] for r in rows]


def live(paths, synth=False):
    import post_census as pc
    import pcap
    rows = table_rows()
    lo, hi = (H(x) for x in EXPECT['generic_queue_range'])
    ps_fn = EXPECT['ps_to_fn']
    for spec in paths:
        label, _, path = spec.partition('=')
        if not path:
            label, path = os.path.basename(spec), spec
        if not os.path.isabs(path) and not os.path.exists(path):
            path = os.path.join(pcap.MAIN, path)
        try:
            cap = pc.Cap(path)
        except (pcap.PError, pc.CensusError, OSError) as e:
            report(False, label + ' input', 'cannot read %s: %s' % (path, e))
            continue
        generic, unnamed, pairs, shapes, wrong = 0, 0, collections.Counter(), collections.Counter(), []
        seen_fns = collections.defaultdict(set)
        n_chain, named_frames = 0, 0
        for fi in range(len(cap.frames)):
            f = cap.frame(fi)
            d = f.draws
            if not len(d):
                continue
            if not f.has_origins:
                report(False, label + ' origins', 'frame %d has no origin table (a version-1 capture cannot be checked)' % fi)
                break
            named_frames += 1
            ps = cap.effs(d['ps_hash'])
            vs = cap.effs(d['vs_hash'])
            cs = f.origins['call_site']
            seq, n_dof, n_restore, fns = [], 0, 0, set()
            for i in range(len(d)):
                if not (vs[i] in pc.CHAIN_VS and (ps[i] in pc.CHAIN_PS or str(ps[i]).startswith(('yGlareEtc/', 'yMonotone/')))):
                    continue
                short = pc.CHAIN_PS.get(ps[i]) or ('combine' if str(ps[i]).startswith('yGlareEtc/') else 'monotone')
                seq.append(short)
                n_dof += short == 'dof'
                n_restore += short == 'copy_depth'
                site = int(cs[i])
                if synth:  # selftest positive control: pretend the hooks ran (every chain draw sits inside its expected effect function)
                    w = ps_fn[short]
                    site = H(w['exact'] if 'exact' in w else w['any_of'][0]) + 4
                n_chain += 1
                if lo <= site < hi:
                    generic += 1
                    continue
                row = row_of(rows, site)
                if not row or not row[3]:
                    unnamed += 1
                    continue
                fn = row[0]
                fns.add(fn)
                pairs[(short, hex(fn))] += 1
                want = ps_fn[short]
                ok = (H(want['exact']) == fn) if 'exact' in want else (fn in {H(x) for x in want['any_of']})
                if not ok:
                    wrong.append('frame %d %s -> %s' % (fi, short, hex(fn)))
            shape = pc.shape_of(seq, n_dof, n_restore)
            shapes[shape] += 1
            if shape in EXPECT['shapes']:
                missing = {H(x) for x in EXPECT['shapes'][shape]['functions']} - fns
                if missing and len(wrong) < 5:
                    wrong.append('frame %d (%s): expected effect functions not seen: %s' % (fi, shape, sorted(hex(x) for x in missing)))
            seen_fns[shape] |= fns
        report(generic == 0 and unnamed == 0 and named_frames > 0, label + ' chain draws named', '%d chain draws, %d on a generic queue site, %d without a technique row, %d frames' % (n_chain, generic, unnamed, named_frames))
        report(not wrong, label + ' names match the shape', '; '.join(wrong[:4]) if wrong else 'shapes %s' % dict(shapes))
        print('     observed (chain shader -> effect function): ' + ', '.join('%s->%s x%d' % (a, b, n) for (a, b), n in sorted(pairs.items())))


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        return 1
    synth = '--synth' in a
    a = [x for x in a if x != '--synth']
    if '--static' in a:
        static()
        a = [x for x in a if x != '--static']
    if a:
        live(a, synth)
    print('%d FAIL' % len(fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
