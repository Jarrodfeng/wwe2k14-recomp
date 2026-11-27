"""qgstate_selftest: q gstate (S111 CDSTATE) re-finds the render-state facts settled from the listings today, with two planted twins that fail.
  live    tools/q/qgstate.function_rows on the real tree: the crowd node's six states and their six restores, the cards' alpha test on / blend on
          and their restores, WATR's z write 0 + blend code 1 (entry 5) and its z write restore, the flash draw's z off and its restore
  twin 1  the value tracer off (trace=False): every set expectation must FAIL (no constant is read)
  twin 2  the restore pairing off (pair=False): every restore expectation must FAIL
  verb    `q gstate sub_82519508 --direct --json` (the q.py hook) and the index: `q gstate --setter alpha_test --value 1 --json` holds the cards
          (+218 = 1) and not the crowd node (its alpha test calls are 0 / restore); the forwarder row sub_8259FAE8 +328 alpha ref 128 via sub_8259E488 r6
Skipped (exit 0, said) when the q index or generated\\ is absent. 0 game runs.
Run:  py -I tools/q/qgstate_selftest.py"""
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import q as qm  # noqa: E402
import qdecomp  # noqa: E402
import qgstate  # noqa: E402

Q = os.path.join(HERE, 'q.py')
fails = []

CROWD, CARDS, WATR, FLASH = 0x82519508, 0x825185B0, 0x8272C198, 0x823B27F0
# (function, offset, key, kind, constant or None, the save offset a restore puts back or None): read in the listings (S111 CROWDATEST,
# JBOYBLEND, FLASHHIDE2 / RSTDIFF and this lane's decomps)
EXPECT = [
    (CROWD, 0x178, 'cull', 'set', 1, None), (CROWD, 0x184, 'blend_code', 'set', 0, None), (CROWD, 0x190, 'blend_enable', 'set', 0, None),
    (CROWD, 0x19C, 'alpha_test', 'set', 0, None), (CROWD, 0x1A8, 'alpha_ref', 'set', 192, None), (CROWD, 0x1B4, 'alpha_func', 'set', 5, None),
    (CROWD, 0x28C, 'cull', 'restore', None, 0x12C), (CROWD, 0x298, 'blend_code', 'restore', None, 0x150),
    (CROWD, 0x2A4, 'blend_enable', 'restore', None, 0x138), (CROWD, 0x2B0, 'alpha_test', 'restore', None, 0x144),
    (CROWD, 0x2BC, 'alpha_ref', 'restore', None, 0x168), (CROWD, 0x2C8, 'alpha_func', 'restore', None, 0x15C),
    (CARDS, 0x200, 'blend_code', 'set', 0, None), (CARDS, 0x20C, 'blend_enable', 'set', 1, None), (CARDS, 0x218, 'alpha_test', 'set', 1, None),
    (CARDS, 0x354, 'blend_code', 'restore', None, 0x170), (CARDS, 0x360, 'blend_enable', 'restore', None, 0x158),
    (CARDS, 0x36C, 'alpha_test', 'restore', None, 0x164), (CARDS, 0x37C, 'sampler_address', 'restore', None, 0x180),
    (WATR, 0x1F8, 'z_enable', 'set', 1, None), (WATR, 0x204, 'z_write', 'set', 0, None), (WATR, 0x228, 'blend_code', 'set', 1, None),
    (WATR, 0x2BC, 'z_write', 'restore', None, 0x1AC),
    (FLASH, 0x354, 'z_enable', 'set', 0, None), (FLASH, 0x384, 'z_write', 'set', 0, None), (FLASH, 0x434, 'z_enable', 'restore', None, 0x2E4),
]


def check(what, ok, detail=''):
    print('%s %s%s' % ('ok  ' if ok else 'FAIL', what, '' if ok else '  ' + str(detail)[:900]))
    if not ok:
        fails.append(what)


def misses(rows_of, expect):
    """-> [the expectations the rows do not hold]"""
    out = []
    for fn, off, key, kind, imm, save in expect:
        r = next((x for x in rows_of[fn] if x['off'] == off and x['key'] == key and x['kind'] != 'read'), None)
        if r is None or r['kind'] != kind:
            out.append((hex(fn), hex(off), key, kind, 'got', r and r['kind']))
            continue
        if kind == 'set' and (r['args'].get(r['value_reg']) or {}).get('imm') != [imm]:
            out.append((hex(fn), hex(off), key, 'value', r['args']))
        if kind == 'restore' and r['restore'] != save:
            out.append((hex(fn), hex(off), key, 'restore', r['restore']))
    return out


def q(*args):
    r = subprocess.run([sys.executable, '-I', Q] + list(args), capture_output=True, text=True, timeout=600)
    return r.returncode, r.stdout, r.stderr


def main():
    if not os.path.exists(qm.DB) or not os.path.isdir(qdecomp.GEN):
        print('skip: no q index / generated\\ on this tree (q gstate reads the listings)')
        return 0
    con = qm.connect(qm.DB)
    setters, getters = qgstate.table()
    fns = sorted({e[0] for e in EXPECT})
    live = {f: qgstate.function_rows(con, f, setters, getters) for f in fns}
    m = misses(live, EXPECT)
    check(f'live: the {len(EXPECT)} settled setter calls (crowd node, cards, WATR, flash) are found with their constants and save -> restore pairs', not m, m)

    sets = [e for e in EXPECT if e[3] == 'set']
    restores = [e for e in EXPECT if e[3] == 'restore']
    twin = {f: qgstate.function_rows(con, f, setters, getters, trace=False) for f in fns}
    mt = misses(twin, sets)
    check(f'twin 1 (value tracer off): all {len(sets)} constant expectations fail', len(mt) == len(sets), f'{len(mt)} of {len(sets)} failed: {mt}')
    twin2 = {f: qgstate.function_rows(con, f, setters, getters, pair=False) for f in fns}
    mp = misses(twin2, restores)
    check(f'twin 2 (restore pairing off): all {len(restores)} restore expectations fail', len(mp) == len(restores), f'{len(mp)} of {len(restores)} failed')

    rc, out, err = q('gstate', 'sub_82519508', '--direct', '--json')
    try:
        rows = json.loads(out)['functions']['0x82519508']['rows']
        ar = next(r for r in rows if r['off'] == 0x1A8)
        ok = rc == 0 and ar['key'] == 'alpha_ref' and ar['args']['r4']['imm'] == [192]
    except (ValueError, KeyError, StopIteration) as e:
        ok, rows = False, repr(e)
    check('verb: q gstate sub_82519508 --direct --json names +1A8 alpha_ref = 192', ok, (rc, err[-400:], out[:400]))

    rc, out, err = q('gstate', '--setter', 'alpha_test', '--value', '1', '--json')
    try:
        d = json.loads(out)
        by = {x['func']: x for x in d['functions']}
        cards = by.get('0x825185B0')
        ok = (rc == 0 and d['setter'] == 'alpha_test' and cards is not None and any(c['off'] == 0x218 and c['args']['r4']['imm'] == [1] for c in cards['calls'])
              and '0x82519508' not in by)
    except (ValueError, KeyError) as e:
        ok = False
        out = repr(e) + out[:300]
    check('index: --setter alpha_test --value 1 holds the cards +218 and not the crowd node', ok, (rc, err[-400:], out[:400]))

    rc, out, err = q('gstate', 'sub_8259FAE8', '--json')
    try:
        rows = json.loads(out)['functions']['0x8259FAE8']['rows']
        fw = next(r for r in rows if r['off'] == 0x328)
        ok = rc == 0 and fw['key'] == 'alpha_ref' and fw['via'] == 0x8259E488 and fw['value_reg'] == 'r6' and fw['args']['r6']['imm'] == [128]
    except (ValueError, KeyError, StopIteration) as e:
        ok = False
        out = repr(e) + out[:300]
    check('index: the forwarder sub_8259E488 (alpha ref = its r6) gives sub_8259FAE8 +328 alpha_ref 128 via it', ok, (rc, err[-400:], out[:400]))

    print('qgstate_selftest: ' + ('FAILED: ' + '; '.join(fails) if fails else 'all passed'))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
