r"""Lane 39 (S101): the files-only duplicate census of the arenas (docs\renderer\ARENA_PROMOTION_MAP.md section 6, lane J). Offline, 0 game runs, no capture.

Question: can a stream draw be joined to a stage slot by what the file holds? Three keys, weakest to strongest, per arena (every bgNN.pac):
  K1 counts     (vertex count, index count as drawn): the join the map calls ambiguous. A group of meshes sharing it is a collision.
  K2 positions  K1 plus the vertex positions exactly: the content join. A group of meshes (of DIFFERENT slots) sharing it cannot be told apart by position content.
  K3 material   K2 plus the index values, ALL the other vertex bytes (normals, colours, UV sets, tangents), the material class, its parameters and the sampler textures' bytes: meshes equal on K3 are interchangeable for a replacement (the same
                picture whichever slot is chosen); K2 groups that differ on K3 need the texture (or the declared asset) to be told apart.
Then per SLOT: slots whose whole mesh list is equal on K2 (indistinguishable by content) and on K3 (interchangeable).

  py -I tools\arena\files_dupes.py [--ids 4,5] [--json OUT] [-q]
Exit 1 when an arena cannot be read or no arena was read. Numbers and slot ids only; nothing from the files is written."""
import argparse
import collections
import hashlib
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import arena_files as af  # noqa: E402


def _h(*parts):
    h = hashlib.blake2b(digest_size=12)
    for p in parts:
        h.update(p if isinstance(p, bytes) else str(p).encode())
        h.update(b'|')
    return h.hexdigest()


def analyse(arena):
    """-> dict of counts and the slot lists of the duplicate classes for one af.Arena."""
    meshes = [m for ms in arena.slots.values() for m in ms]
    ok = [m for m in meshes if m.positions is not None]
    k1, k2, k3 = collections.defaultdict(list), collections.defaultdict(list), collections.defaultdict(list)
    sig2, sig3 = {}, {}
    for m in ok:
        pos = hashlib.blake2b(m.positions.tobytes(), digest_size=12).digest()
        tex = []
        for key in sorted(m.samplers):
            t = arena.textures.get(m.samplers[key].lower())
            tex.append((key, hashlib.blake2b(t.blob, digest_size=8).digest() if t else m.samplers[key]))
        s1 = (m.n, m.stream_index_count)
        s2 = s1 + (pos,)
        s3 = s2 + (hashlib.blake2b(m.indices.tobytes(), digest_size=8).digest(), m.effect, tuple(tex), m.stream_hash, repr(sorted(m.params.items())))
        k1[s1].append(m), k2[s2].append(m), k3[s3].append(m)
        sig2[(m.slot, m.index)], sig3[(m.slot, m.index)] = s2, s3

    def classes(groups):
        """groups with meshes in more than one slot (two meshes of one slot sharing a key are one slot's own repetition, listed apart)."""
        out, same_slot = [], 0
        for key, ms in groups.items():
            slots = sorted({m.slot for m in ms})
            if len(slots) > 1:
                out.append(slots)
            elif len(ms) > 1:
                same_slot += 1
        return out, same_slot
    c1, _ = classes(k1)
    c2, _ = classes(k2)
    c3, _ = classes(k3)
    # per slot: the whole mesh list
    def slot_classes(sig):
        by = collections.defaultdict(list)
        for k, ms in arena.slots.items():
            if all(m.positions is not None for m in ms):
                by[tuple(sig[(m.slot, m.index)] for m in ms)].append(k)
        return sorted(v for v in by.values() if len(v) > 1)
    s2c, s3c = slot_classes(sig2), slot_classes(sig3)
    return {
        'slots': len(arena.slots), 'meshes': len(meshes), 'meshes_untiled_skipped': len(meshes) - len(ok),
        'k1_collision_groups': len(c1), 'k2_groups_across_slots': len(c2), 'k3_groups_across_slots': len(c3),
        'k2_groups_split_by_k3': sum(1 for ms in k2.values() if len({m.slot for m in ms}) > 1 and len({sig3[(m.slot, m.index)] for m in ms}) > 1),
        'slot_classes_k2': s2c, 'slot_classes_k3': s3c,
        'slots_in_k2_classes': sum(len(c) for c in s2c), 'slots_in_k3_classes': sum(len(c) for c in s3c),
        'largest_k3_slot_class': max([len(c) for c in s3c] or [0]),
        'k2_groups': c2,
    }


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--ids', default='')
    ap.add_argument('--json', default='')
    ap.add_argument('-q', action='store_true')
    a = ap.parse_args(argv)
    try:
        ids = [int(x) for x in a.ids.split(',') if x] or af.arena_ids()
    except af.FileReadError as e:
        print('FAIL  %s' % e)
        return 1
    out, bad = {}, 0
    tot = collections.Counter()
    for aid in ids:
        try:
            r = analyse(af.load_arena(aid))
        except Exception as e:
            print('FAIL  bg%02d: %s' % (aid, e))
            bad += 1
            continue
        out[aid] = r
        for k in ('slots', 'meshes', 'meshes_untiled_skipped', 'k1_collision_groups', 'k2_groups_across_slots', 'k3_groups_across_slots', 'k2_groups_split_by_k3', 'slots_in_k2_classes', 'slots_in_k3_classes'):
            tot[k] += r[k]
        tot['arenas_with_k2_classes'] += 1 if r['slot_classes_k2'] else 0
        tot['arenas_with_k3_classes'] += 1 if r['slot_classes_k3'] else 0
        if not a.q:
            print('bg%02d  slots %3d  meshes %4d (untiled %d)  K1 collisions %3d  K2 groups %3d  K3 groups %3d  | slots in K2 classes %3d, K3 classes %3d (largest %d)'
                  % (aid, r['slots'], r['meshes'], r['meshes_untiled_skipped'], r['k1_collision_groups'], r['k2_groups_across_slots'], r['k3_groups_across_slots'],
                     r['slots_in_k2_classes'], r['slots_in_k3_classes'], r['largest_k3_slot_class']))
    if not out:
        print('FAIL  no arena was read (a census that reads nothing must fail)')
        return 1
    print('files_dupes: %d arenas read; K1 collision groups %d; K2 groups across slots %d; K3 groups %d (K3 splits K2 groups: %d of them split); arenas with K2 slot classes %d, with K3 classes %d; slots in K3 classes %d of %d'
          % (len(out), tot['k1_collision_groups'], tot['k2_groups_across_slots'], tot['k3_groups_across_slots'], tot['k2_groups_split_by_k3'], tot['arenas_with_k2_classes'], tot['arenas_with_k3_classes'],
             tot['slots_in_k3_classes'], tot['slots']))
    if a.json:
        os.makedirs(os.path.dirname(os.path.abspath(a.json)), exist_ok=True)
        json.dump({str(k): v for k, v in out.items()}, open(a.json, 'w'), indent=1)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
