"""S101 lane 4: cross-check the drawables atlas against the viewer's census (asset_dump --all --json).

    py -I tools\\atlas\\crosscheck.py --atlas OUT\\atlas.json --census CENSUS.json [--append OUT\\summary.md]

For each wrestler attire / arena the census lists (meshes, textures_loaded), count what the atlas lists for the same pac entries and
report equal / differing items, one line per difference with the known cause. Both inputs are local game-derived files; the output
holds pac names and counts only. Exit 0 always (it is a report); a missing or empty input is an error (exit 2).
"""
import argparse
import collections
import json
import re
import sys


def load_atlas(path):
    meshes = collections.defaultdict(list)  # pac -> mesh records
    for line in open(path, encoding='utf-8'):
        line = line.strip().rstrip(',')
        if not line.startswith('{'):
            continue
        r = json.loads(line)
        if r['type'] == 'mesh':
            meshes[r['pac']].append(r)
    return meshes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--atlas', required=True)
    ap.add_argument('--census', required=True)
    ap.add_argument('--append', default='')
    a = ap.parse_args()
    meshes = load_atlas(a.atlas)
    census = json.load(open(a.census, encoding='utf-8'))
    if not meshes or not census:
        print('FAIL: empty atlas or census', file=sys.stderr)
        return 2
    ring_keys = {951, 952, 953, 960, 964, 973, 974}
    rows = []  # (label, atlas_meshes, census_meshes, note)
    equal = more = fewer = 0
    for it in census:
        label = it['label']
        m = re.match(r'wrestler (\d+) attire (\d+)', label)
        n = re.match(r'arena (\d+)', label)
        if m:
            pac = 'pac/ch/ch%03d.pac' % int(m.group(1))
            recs = [r for r in meshes.get(pac, []) if r['loaded'] and r['entry'].count('/') == 1]
            by_p = collections.OrderedDict()
            for r in recs:
                by_p.setdefault(r['entry'].split('/')[0], []).append(r)
            # attires = the PACHs that hold a model, in order
            order = sorted(by_p, key=lambda s: int(s[1:]))
            at = int(m.group(2))
            have = len(by_p[order[at]]) if at < len(order) else 0
            note = 'census loads this attire from the PACH\'s pieces; the atlas counts every loaded mesh of its top-level entries'
        elif n:
            pac = 'pac/bg/bg%02d.pac' % int(n.group(1))
            recs = [r for r in meshes.get(pac, []) if r['loaded'] and r['entry'].count('/') == 1]
            have = 0
            for r in recs:
                key = int(r['entry'].split('/')[1])
                if key < 400 or key in ring_keys:
                    have += 1
            note = 'census adds crowd bodies, props and stage screens and drops a few slots (titan_OFF, 956); the atlas counts slots 0-399 + the ring keys'
        else:
            continue
        want = it['meshes']
        if have == want:
            equal += 1
        else:
            if want > have:
                more += 1
            else:
                fewer += 1
            rows.append((label, have, want))
    out = []
    out.append('\n## Census cross-check (tools\\atlas\\crosscheck.py; census = asset_dump --all --json)\n')
    out.append('Items in the census: %d. Same mesh count in the atlas: %d. Census has MORE meshes than the atlas lists for the same entries: %d. Census has FEWER: %d.\n' % (len(census), equal, more, fewer))
    out.append('"More" is expected for arenas (the census loader adds crowd bodies, props, stage screens and ring copies the atlas lists under their own pac entries); '
               '"fewer" is the loader dropping slots on purpose (stage-off copies, the unknown bar 956) or an atlas gap, and each is listed. '
               'Textures: the census counts textures the loader actually loaded per item (a deduplicated set, shared textures included); the atlas lists every texture of every directory once, '
               'so the two are not the same quantity and are not compared item by item.\n')
    out.append('Differences (label: atlas / census):\n')
    for label, have, want in rows[:400]:
        out.append('- %s: %d / %d (%s)' % (label, have, want, 'census more' if want > have else 'census FEWER'))
    if len(rows) > 400:
        out.append('- ... %d more' % (len(rows) - 400))
    text = '\n'.join(out) + '\n'
    print(text[:3000])
    if a.append:
        s = open(a.append, encoding='utf-8').read()
        k = s.find('\n## Census cross-check')
        if k >= 0:
            s = s[:k]
        open(a.append, 'w', encoding='utf-8').write(s.rstrip('\n') + '\n' + text)
    return 0


if __name__ == '__main__':
    sys.exit(main())
