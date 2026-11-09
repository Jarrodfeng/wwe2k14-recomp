"""Session 98: a Python re-implementation of the game hook's patcher (src\\lighting_mod.h ApplyPatches / FindChunk), run on the
player's own decoded arena light key (entry 30000, read-only).  Used to check a lighting mod file (docs\\systems\\lighting_mod.md):
the file's key_fnv must equal the key's FNV, and the patches, applied here, must give the edited values when decoded with
tools\\assets\\stagekey.py (eval_set).

    py -I tools\\assets\\lighting_mod_apply.py --arena 5 --mod path\\arena_05.json            check the hash, apply, print every patched track
    py -I tools\\assets\\lighting_mod_apply.py --arena 5 --mod FILE --set 121 --frame 132     also print the evaluated set (5 dirs, colours, sky, ground)
    py -I tools\\assets\\lighting_mod_apply.py --arena 5 --mod FILE --expect-scale 121 4095 0 0   assert every patched scale of set 121 (lights) is [4095,0,0]

Nothing from the game is written: the patched bytes stay in memory.
"""
import argparse
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)  # -I drops the script folder from sys.path
import stagekey as sk  # noqa: E402


def fnv1a64(b):
    h = 0xcbf29ce484222325
    for c in b:
        h = ((h ^ c) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def find_chunk(d, k, act, track, ordinal):
    """Offset of the payload (after the optional prefix) of the `ordinal`-th chunk of >= 25 bytes of record (act, track), or -1.
    Same walk as FindChunk in src\\lighting_mod.h."""
    r = k.by.get((act, track))
    if r is None:
        return -1
    end = k.rec[r[0] + 1][3] if r[0] + 1 < k.n else len(d) - k.base
    p, e, seen = k.base + r[3], k.base + end, 0
    while p < e:
        t = d[p]
        if t < 96:
            p += 1
            continue
        if t == 96:
            ln, q = d[p + 1] | d[p + 2] << 8, p + 3
        else:
            ln, q = t - 96, p + 1
        if q + ln > len(d):
            return -1
        at, l = q, ln
        if l and d[at] & 0x80:
            skip = (d[at] & 0x7F) + 1
            if skip > l:
                return -1
            at, l = at + skip, l - skip
        if l >= 25:
            if seen == ordinal:
                return at
            seen += 1
        p = q + ln
    return -1


def apply_patches(data, patches):
    """-> (patched bytearray, applied, skipped)"""
    b = bytearray(data)
    k = sk.YMKs(bytes(b))
    applied = skipped = 0
    for p in patches:
        at = find_chunk(b, k, p['act'], p['track'], p.get('chunk', 0))
        if at < 0 or at + 25 > len(b):
            skipped += 1
            continue
        if 'pos' in p:
            struct.pack_into('<3f', b, at + 1, *p['pos'])
        if 'angle' in p:
            struct.pack_into('<3h', b, at + 13, *[max(-32768, min(32767, int(round(v)))) for v in p['angle']])
        if 'scale' in p:
            struct.pack_into('<3h', b, at + 19, *[max(-32768, min(32767, int(round(v)))) for v in p['scale']])
        applied += 1
    return b, applied, skipped


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--arena', type=int, required=True)
    ap.add_argument('--mod', required=True)
    ap.add_argument('--set', type=int)
    ap.add_argument('--frame', type=float, default=132)
    ap.add_argument('--expect-scale', nargs=4, type=int, metavar=('SET', 'R', 'G', 'B'))
    a = ap.parse_args()
    mod = json.load(open(a.mod))
    d = bytes(sk.arena_entries(a.arena)['p0/30000'])
    fnv, fd = fnv1a64(d), fnv1a64(d[sk.YMKs(d).base:])
    ok_fnv = int(mod['key_fnv'], 16) == fnv
    ok_data = int(mod.get('key_fnv_data', '0x0'), 16) == fd
    ok_size = mod.get('key_size', len(d)) == len(d)
    print('key_fnv %s (mod %s, key 0x%016X)  key_fnv_data %s  key_size %s' % (
        'MATCH' if ok_fnv else 'MISMATCH', mod['key_fnv'], fnv, 'MATCH' if ok_data else 'MISMATCH', 'ok' if ok_size else 'MISMATCH'))
    b, applied, skipped = apply_patches(d, mod['patches'])
    print('patches: %d applied, %d skipped, %d bytes changed' % (applied, skipped, sum(1 for x, y in zip(d, b) if x != y)))
    k = sk.YMKs(bytes(b))
    sets = sorted(set(p['act'] for p in mod['patches']))
    for sid in ([a.set] if a.set is not None else sets):
        e = sk.eval_set(k, sid, a.frame)
        print('set %d @frame %g:' % (sid, a.frame))
        for i in range(5):
            print('  light %d dir %s col %s' % (i, [round(v, 4) for v in e['dir'][i]], [round(v, 4) for v in e['col'][i]]))
        print('  sky %s ground %s' % ([round(v, 4) for v in e['sky']], [round(v, 4) for v in e['ground']]))
    bad = 0
    for p in mod['patches']:
        _, keys = sk.light_keys(k, p['act'], p['track'])
        if p.get('chunk', 0) >= len(keys):
            bad += 1
            continue
        _, _, pos, ang, sc = keys[p['chunk']]
        if 'angle' in p and list(ang) != list(p['angle']):
            bad += 1
        if 'scale' in p and list(sc) != list(p['scale']):
            bad += 1
    print('decoded patched keys equal the patch numbers: %s (%d mismatches)' % ('YES' if bad == 0 else 'NO', bad))
    if a.expect_scale:
        sid, want = a.expect_scale[0], a.expect_scale[1:]
        for tr in (10, 11, 12, 13):
            _, keys = sk.light_keys(k, sid, tr)
            for key in keys:
                if list(key[4]) != want:
                    print('EXPECT FAIL set %d track %d scale %s != %s' % (sid, tr, list(key[4]), want))
                    sys.exit(1)
        print('expect-scale ok: every key of set %d lights 0-3 is %s' % (sid, want))
    sys.exit(0 if (ok_fnv and ok_size and bad == 0 and skipped == 0) else 1)


if __name__ == '__main__':
    main()
