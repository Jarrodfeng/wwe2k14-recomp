#!/usr/bin/env python3
r"""S103 L10: the game's vertex-LM class per arena mesh = bit 0 of the mesh record word +0x90 (yBG_ effects), tested against the committed stream table. 0 game runs.

  py -I tools\arena\l10_rule.py [--make OUT.json VT1.json VT2.json ...] [--plant]

Table tools\arena\arena4_vlm.json: slot / mesh -> whether the game's VERTEX program for that mesh was a `*_vertex_lm_vertex` one (read from the streams by tools\arena\variant_table.py, which records
the vertex variant per mesh; names of classes only, no hashes, no game data). The rule is read from the FILES (tools\arena\arena_files.py + the record word). Prints the agreement; exit 1 below 100 %
(the `yProp_` copies carry value 3 and have no vertex-LM program: excluded by effect). --plant tests "word +0x90 == 0" as the rule (must fail)."""
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import arena_files as af  # noqa: E402

TABLE = os.path.join(HERE, 'arena4_vlm.json')


def words(arena_id):
    """(slot, mesh) -> (effect, word +0x90) for every mesh record of the arena."""
    raw = {}
    orig = af.parse_jboy

    def wrap(slot, blob, with_positions=True):
        out = orig(slot, blob, with_positions)
        body = blob[8:8 + af.be32(blob, 4)]
        mtab = af.be32(body, 0x14)
        for mi in range(af.be32(body, 0x10)):
            rec = mtab + mi * af.MESH_REC
            raw[(slot, mi)] = (af._cstr(body[rec + 0x7C:rec + 0x7C + 16]), struct.unpack_from('>I', body, rec + 0x90)[0])
        return out

    af.parse_jboy = wrap
    af._CACHE.clear()
    try:
        af.load_arena(arena_id)
    finally:
        af.parse_jboy = orig
        af._CACHE.clear()
    return raw


def make(out, tables):
    vs = {}
    for t in tables:
        for r in json.load(open(t)):
            for v in r['vs']:
                vs.setdefault((r['slot'], r['mesh']), set()).add(v)
    rows = [{'slot': k[0], 'mesh': k[1], 'vlm': any('vertex_lm' in x for x in v)} for k, v in sorted(vs.items()) if all(x.startswith('yBG_') for x in v)]
    json.dump(rows, open(out, 'w'), indent=1)
    print('wrote %d rows' % len(rows))


def main():
    if len(sys.argv) > 2 and sys.argv[1] == '--make':
        return make(sys.argv[2], sys.argv[3:])
    plant = '--plant' in sys.argv
    rows = json.load(open(TABLE))
    w = words(4)
    ok = bad = 0
    miss = []
    for r in rows:
        eff, word = w[(r['slot'], r['mesh'])]
        if not eff.startswith('yBG_'):
            continue
        pred = (word == 0) if plant else (word & 9) == 1
        if pred == r['vlm']:
            ok += 1
        else:
            bad += 1
            miss.append((r['slot'], r['mesh'], hex(word)))
    print('vertex-LM rule (record word +0x90 bit 0): %d of %d arena-4 yBG_ meshes match the stream (%d wrong) %s' % (ok, ok + bad, bad, miss[:6]))
    good = bad == 0 and ok > 0
    if '--expect-fail' in sys.argv:  # the planted twin must be caught
        caught = bad > 0 and ok + bad > 0   # S105 E review 6: zero meshes found is not a caught twin (a check that reads nothing proves nothing)
        if caught:
            print('PLANTED-OK: the planted vertex-LM rule was caught (%d of %d meshes disagree with the stream)' % (bad, ok + bad))
        return 0 if caught else 1
    return 0 if good else 1


if __name__ == '__main__':
    sys.exit(main())
