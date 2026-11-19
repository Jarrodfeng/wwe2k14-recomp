r"""S103 F5: the particle blend code from its WRITER (S103_FABLE_REVIEW 1.6: ask who writes a value before learning it from its readers). Read-only, prints numbers and emitter names only.

  py -I tools\scene\f5_blend_codes.py [--check]

Writer [C: listing of sub_82A10760 (the emitter loader) 0x82A107A0-0x82A107C4 + sub_82A0C470]: the loader reads the 48-byte emitter header of the 'rootchk' block (file offsets +8 .. +56 of
the block) into the emitter at +192 (sub_82A10EE8), then sub_82A0C470 byte-swaps its fields in place. So emitter+192 = block+8 (MODE), **emitter+196 = block+12 (u32)** = the item's blend code
(the draw reads *(*(item+48))+196), emitter+200 = block+16 (the 16-byte name, "Light" in the run-22 dump), emitter+216 / +218 = block+32 / +34 (max / command count, which the same listing reads with lhz 216 / 218).
So the blend code is DATA of the player's own prtcls.pac: this tool lists the u32 at block+12 of every emitter of every effect, with the mode and the sprite texture of the effect (cmd 22), and checks
that (a) the value is 0 / 1 / 2 only, (b) the run-22 emitter ("Light", mode 0, texture Light0001) holds 1.
"""
import collections, os, struct, sys
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'assets'))
import prtc  # noqa: E402
sys.path.insert(0, os.path.join(prtc.MAIN, 'tools', 'wm30'))
import pac  # noqa: E402


def emitters():
    """[(bank, effect, emitter name, mode, blend, max, texture or '')]"""
    d = pac.load(prtc.PAC)
    out = []
    for sec, nm, at in prtc.directory(d):
        if sec != 'PCE6':
            continue
        for eid, raw0 in pac.pach(d, at):
            raw = pac.decode(raw0)
            if raw[:4] != b'PACH':
                continue
            sub = dict(pac.pach(raw))
            if 0 not in sub:
                continue
            body = pac.decode(sub[0])
            recs = prtc.records(body)
            # split by emitter: a 'root' then its cmds
            cur = None
            for r in recs:
                if r[0] == 'root':
                    at_ = r[2]
                    mode, blend = struct.unpack_from('<II', body, at_ + 8)
                    mx = struct.unpack_from('<H', body, at_ + 32)[0]
                    cur = [nm, eid, r[1].split(' (')[0], mode, blend, mx, '']
                    out.append(cur)
                elif r[0] == 'cmd' and r[1] == 22 and cur is not None:
                    cur[6] = r[5]
    return [tuple(x) for x in out]


def main():
    em = emitters()
    print('emitters', len(em))
    hist = collections.Counter(e[4] for e in em)
    print('blend code histogram', dict(sorted(hist.items())))
    by = collections.defaultdict(collections.Counter)
    for bank, eid, name, mode, blend, mx, tex in em:
        stem = ''.join(c for c in tex.split('.')[0] if not c.isdigit()).rstrip('_') or '(none)'
        by[stem][blend] += 1
    for stem, c in sorted(by.items(), key=lambda kv: -sum(kv[1].values())):
        print('  texture stem %-12s codes %s' % (stem, dict(sorted(c.items()))))
    bym = collections.defaultdict(collections.Counter)
    for e in em:
        bym[e[3]][e[4]] += 1
    print('by mode:', {m: dict(sorted(c.items())) for m, c in sorted(bym.items())})
    for e in em:
        if e[4] not in (0, 1, 2):
            print('OTHER CODE', e)
    light = [e for e in em if e[2] == 'Light']
    print('"Light" emitters: %d, codes %s, modes %s, textures %s' % (len(light), sorted({e[4] for e in light}), sorted({e[3] for e in light}), sorted({e[6] for e in light})[:6]))
    if '--check' in sys.argv:
        k = [e for e in em if e[0] == '0107']
        smoke = [e for e in k if e[2] == 'Smoke_UP']
        fire = [e for e in k if e[2] == 'Fire01']
        ok = all(e[4] in (0, 1, 2) for e in em) and smoke and fire and all(e[4] == 0 for e in smoke) and all(e[4] == 1 for e in fire) and any(e[4] == 1 for e in light)
        if '--plant' in sys.argv and sys.argv[sys.argv.index('--plant') + 1] == 'mode_field':   # the wrong field (block+8 = the mode) read as the blend code
            ok = all(e[3] == 0 for e in smoke) and all(e[3] == 1 for e in fire)
        print('CHECK', 'PASS' if ok else 'FAIL')
        return 0 if ok else 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
