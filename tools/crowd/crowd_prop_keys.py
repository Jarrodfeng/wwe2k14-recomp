#!/usr/bin/env python3
r"""S103 lane SG, offline, 0 game runs: what the arena pacs' audience set (bgNN.pac, PACH 0, entry 20000) holds in the key range 40000-42999, and
whether the keys the game's prop loaders name exist (static_SetUpSignBoard sub_8250ACA8: 40000, 40100, 40001; static_SetUpHandSignBoard sub_8250B1E8:
mesh 40999 + h, picture 42000 + h - 1, h 1..19; out\s103\decomp\claims_Q2.jsonl / claims_V2.jsonl). Optionally joins capture images by bytes to those entries.

  py -I tools\crowd\crowd_prop_keys.py [--arena N ...] [--capture DIR --image ID ...] [--plant missing_key]

Prints per arena: the keys found and their kind (mesh: vertices / indices; texture: w x h, format; other: the tag), then the checks:
  keys     every arena with the set holds 40000, 40001, and 40999..41018, 42000..42018 (the loaders' whole range) - or says which are missing;
  join     each --image (capture slot-0 content) equals the mip-0 bytes of one texture entry of the range (names it), else 'not found'.
Only names, sizes and counts are printed; nothing from the game is written. Fast: decodes only entry 20000 of PACH 0, not the whole pac.
--plant missing_key: key 41016 is dropped from the listing; `keys` must FAIL."""
import argparse
import glob
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path[:0] = [os.path.join(REPO, 'tools', 'wm30'), os.path.join(REPO, 'tools', 'solver'), os.path.join(REPO, 'tools', 'ui'), os.path.join(REPO, 'tools', 'arena')]


def main_checkout():
    import subprocess
    try:
        common = subprocess.run(['git', '-C', REPO, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return REPO


MAIN = main_checkout()
GAME = os.path.join(MAIN, 'game_data', 'extracted')
WANT = [40000, 40001] + list(range(41000, 41019)) + list(range(42000, 42019))


def be32(b, o):
    return struct.unpack_from('>I', b, o)[0]


def audience_children(path):
    """[(key, decoded leaf)] of PACH 0 entry 20000, keys 40000-42999 (only those leaves are decoded)."""
    import pac
    data = pac.load(path)
    pos = 0x4000 if data[:4] == b'EPAC' else 0
    while pos + 8 <= len(data) and data[pos:pos + 4] != b'PACH':
        pos += 0x800
    if data[pos:pos + 4] != b'PACH':
        return None
    for eid, raw in pac.pach(data, pos):
        if eid != 20000:
            continue
        dec = pac.decode(raw)
        if dec[:4] != b'PACH':
            return None
        out = []
        for k, leaf in pac.pach(dec):
            if 40000 <= k < 43000:
                out.append((k, pac.decode(leaf)))
        return out
    return None


def kind_of(leaf):
    if leaf[:4] == b'JBOY':
        body = leaf[8:]
        try:
            if be32(body, 0x10) == 1:
                rec = be32(body, 0x14)
                n = be32(body, rec)
                ix = be32(body, rec + 0x9C)
                cnt = be32(body, ix + 4) if ix and ix + 8 <= len(body) else -1
                return 'mesh', '%d v / %d i' % (n, cnt)
            return 'mesh', '%d meshes' % be32(body, 0x10)
        except struct.error:
            return 'mesh', '?'
    try:
        import ui_pacs as up
        lay = up.texture_directory(leaf)
        if lay:
            return 'texture', '%d textures: %s' % (len(lay), ', '.join('%sx%s %s' % (t.get('w'), t.get('h'), t.get('fmt')) for t in lay[:3]))
    except Exception:
        pass
    i = leaf.find(b'DDS ')
    if 0 <= i < 256:
        h, w = struct.unpack_from('<II', leaf, i + 12)
        fourcc = leaf[i + 84:i + 88]
        return 'texture', '%dx%d %s (DDS at +%d)' % (w, h, fourcc.decode('latin-1'), i)
    return 'other', repr(leaf[:4])


def dds_mip0(leaf):
    """[(offset, w, h, fourcc, mip0 bytes)] of every DDS in the leaf."""
    out = []
    i = leaf.find(b'DDS ')
    while i >= 0:
        try:
            h, w = struct.unpack_from('<II', leaf, i + 12)
            fourcc = leaf[i + 84:i + 88]
            bs = 8 if fourcc == b'DXT1' else 16
            n = max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * bs
            out.append((i, w, h, fourcc, bytes(leaf[i + 128:i + 128 + n])))
        except struct.error:
            pass
        i = leaf.find(b'DDS ', i + 4)
    return out


def capture_blob(cap_dir, image):
    """The captured content bytes of image `image` (its latest RCTX blob), via tools\\ui\\ui_join_census.blob_info."""
    import join_census as jc
    import ui_join_census as uj
    cap = jc.Capture(cap_dir)
    info = None
    for i in range(len(cap.frames)):
        f = cap.frame(i)
        cap.advance_contents(f, i)
        if image in cap.contents:
            info = uj.blob_info(cap, image, {})
    if not info:
        return None, None
    return bytes(cap.tex[info['off']:info['off'] + info['db']]), info


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--arena', type=int, action='append')
    ap.add_argument('--capture')
    ap.add_argument('--image', type=int, action='append', default=[])
    ap.add_argument('--pac', action='append', default=[], help=r'extra pac (relative to game_data\extracted) whose DDS leaves join too, e.g. pac\audience.pac')
    ap.add_argument('--plant', choices=('missing_key',))
    ap.add_argument('-q', action='store_true')
    a = ap.parse_args()
    files = sorted(glob.glob(os.path.join(GAME, 'pac', 'bg', 'bg[0-9][0-9].pac')))
    if a.arena:
        files = [f for f in files if int(os.path.basename(f)[2:4]) in a.arena]
    problems, sets = [], {}
    for f in files:
        arena = int(os.path.basename(f)[2:4])
        try:
            ch = audience_children(f)
        except Exception as e:
            problems.append('arena %d: %s' % (arena, e))
            continue
        if not ch:
            continue
        if a.plant == 'missing_key':
            ch = [(k, l) for k, l in ch if k != 41016]
        sets[arena] = ch
        have = {k for k, _ in ch}
        miss = [k for k in WANT if k not in have]
        if not a.q:
            print('arena %02d: %d keys in 40000-42999; missing of the loaders\' range: %s' % (arena, len(ch), miss or 'none'))
            for k, leaf in ch:
                kd, txt = kind_of(leaf)
                print('    %5d  %-7s %s' % (k, kd, txt))
        if miss:
            problems.append('arena %d lacks %s' % (arena, miss))
        if 40999 in have:   # the hand loader's h > 19 case (key 40999): absent means that wrestler's hand is skipped, b not counted (0x8250B3A8 beq on -1)
            problems.append('arena %d holds 40999 (an h > 19 hand would load it)' % arena)
    print('%s  keys     %d arenas with the audience set; %s' % ('PASS' if sets and not problems else 'FAIL', len(sets), '; '.join(problems[:6]) or 'every one holds 40000, 40001, 41000-41018, 42000-42018 and none holds 40999'))
    bad = bool(problems) or not sets
    extra = []   # --pac: every leaf of these pacs is a join candidate too (the sign pictures live in pac\audience.pac, S103 SG)
    for rel in a.pac:
        import pac
        for nm, leaf in pac.entries(pac.load(os.path.join(GAME, rel)), 6):
            if b'DDS ' in leaf[:4096]:
                extra.append((rel + ':' + nm, leaf))
    if a.capture and a.image:
        cap = a.capture if os.path.isabs(a.capture) else os.path.join(MAIN, a.capture)
        for im in a.image:
            blob, info = capture_blob(cap, im)
            if blob is None:
                print('INFO  join     image %d: no captured content in this capture' % im)
                continue
            hits = []
            for arena, ch in sets.items():
                for k, leaf in ch:
                    for off, w, h, cc, mip0 in dds_mip0(leaf):
                        if mip0 and blob[:len(mip0)] == mip0:
                            hits.append(('bg%02d' % arena, k, off, w, h, cc.decode('latin-1')))
            for nm, leaf in extra:
                for off, w, h, cc, mip0 in dds_mip0(leaf):
                    if mip0 and blob[:len(mip0)] == mip0:
                        hits.append((nm.split(':')[0], nm.split(':')[1], off, w, h, cc.decode('latin-1')))
            keys = sorted({str(h[1]) for h in hits})
            ok = bool(hits)
            bad = bad or not ok
            print('%s  join     image %d (%sx%s fmt %s, %d bytes): %s' % ('PASS' if ok else 'FAIL', im, info.get('w'), info.get('h'), info.get('fmt'), len(blob),
                                                                          ('entry %s in %d pacs (%dx%d %s, DDS at +%d)' % (keys, len({h[0] for h in hits}), hits[0][3], hits[0][4], hits[0][5], hits[0][2])) if ok else 'not found'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
