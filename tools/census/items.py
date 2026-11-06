"""Session 92 census: the item list (deterministic, seeded) -> out\\census\\items.json.

What it answers: "what does the sweep play?" Families:
  roster  every wrestler (db\\ids\\characters.json kind wrestler + db\\ids\\dlc_characters.json) in six-mans of 6, each
          in a different arena, full entrances, then match time; (follow-up 1v1s for wrestlers without an entrance in
          the six-man are added by run.py at run time, ids fu_<id>)
  arena   every arena the six-mans did not use: a 1v1, full entrances
  type    all match types of db\\ids\\match_types.json, entrances skipped with START, 20 s of match
  wm30    one 30 Years match per era (the era's first), with its entrances (--wwe2k14_wm30=N)
  mode    navigator tour, atlas nodes in chunks of 20, Jarrod's recording (normal speed), a CPU 1v1 to its finish
Item: {"id", "family", "spec", "flags": [...], "timeout_s", ...family fields}.

  py tools\\census\\items.py            writes out\\census\\items.json, prints the counts per family
"""
import json
import os
import random
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
IDS = os.path.join(REPO, 'db', 'ids')
OUT = os.path.join(REPO, 'out', 'census')
SEED = 92
MATCH_FRAMES = 1300     # guest frames of match after the bell (>= one full 600-frame log window), ~21 s
TYPE_FRAMES = 1200      # 20 s


def load(name):
    with open(os.path.join(IDS, name), encoding='utf-8') as f:
        return json.load(f)['entries']


def type_count(path):
    """How many wrestlers a match type needs, from its menu path (None = Royal Rumble)."""
    p = path.upper()
    if 'ROYAL RUMBLE' in p:
        return None
    if 'GAUNTLET' in p:
        return 4
    if 'TWO ON THREE' in p:
        return 5
    if 'ONE ON THREE' in p:
        return 4
    if 'ONE ON TWO' in p or 'TRIPLE THREAT' in p:
        return 3
    if 'FATAL 4-WAY' in p or 'TWO ON TWO' in p:
        return 4
    if '6-MAN' in p:
        return 6
    return 2


def main():
    rnd = random.Random(SEED)
    chars = [e for e in load('characters.json') if e.get('kind') == 'wrestler']
    dlc = load('dlc_characters.json')
    names = {e['id']: e['name'] for e in chars + dlc}
    ids = sorted(e['id'] for e in chars) + sorted(e['id'] for e in dlc)
    groups = [ids[i:i + 6] for i in range(0, len(ids), 6)]
    if len(groups[-1]) < 6:
        used = [i for g in groups[:-1] for i in g]
        groups[-1] += rnd.sample([i for i in used if i not in groups[-1]], 6 - len(groups[-1]))
    arenas = [e['id'] for e in load('arenas.json')]
    shuffled = arenas[:]
    rnd.shuffle(shuffled)
    items = []

    def spec_of(wr, typ, arena=None, extra=''):
        s = ', '.join('#%d' % i for i in wr) + '; type=%s' % typ
        if arena is not None:
            s += '; arena=%d' % arena
        return s + '; pace=quick; players=0' + extra

    used_arenas = set()
    for n, g in enumerate(groups):
        a = shuffled[n % len(shuffled)]
        used_arenas.add(a)
        items.append({'id': 'six_%02d' % n, 'family': 'roster', 'spec': spec_of(g, 8, a), 'wrestlers': g, 'arena': a,
                      'names': [names.get(i, str(i)) for i in g], 'entrances': 'full', 'hold_frames': MATCH_FRAMES,
                      'flags': [], 'timeout_s': 2700})
    pool = [i for i in ids]
    for a in arenas:
        if a in used_arenas:
            continue
        w = rnd.sample(pool, 2)
        items.append({'id': 'arena_%02d' % a, 'family': 'arena', 'spec': spec_of(w, 0, a), 'wrestlers': w, 'arena': a,
                      'names': [names.get(i, str(i)) for i in w], 'entrances': 'full', 'hold_frames': MATCH_FRAMES,
                      'flags': [], 'timeout_s': 1800})
    star = []
    for want in ('Daniel Bryan', 'Brock Lesnar', 'John Cena', 'Randy Orton', 'Punk', 'Sheamus', 'Ryback', 'Mark Henry',
                 'Kane', 'Big Show'):
        hit = [e['id'] for e in chars if want.lower() in e['name'].lower()]
        if hit:
            star.append(hit[0])
    for e in load('match_types.json'):
        path = e['menu_paths'][0]
        cnt = type_count(path)
        label = re.sub(r'[^a-z0-9]+', '_', path.lower().replace('play > ', '')).strip('_')
        it = {'id': 'type_%02d' % e['id'], 'family': 'type', 'type_id': e['id'], 'type_name': path,
              'entrances': 'skip', 'hold_frames': TYPE_FRAMES, 'flags': [], 'timeout_s': 900, 'label': label}
        if cnt is None:
            n = int(re.search(r'(\d+)-MAN', path.upper()).group(1))
            it['spec'] = 'royal rumble; type=%d; rumble=%d random; pace=quick; players=0' % (e['id'], n)
            it['rumble'] = n
        else:
            it['spec'] = spec_of(star[:cnt], e['id'])
            it['wrestlers'] = star[:cnt]
        items.append(it)
    # 30 Years: the first match of each era
    lst = os.path.join(OUT, 'wm30_list.json')
    if not os.path.exists(lst):
        subprocess.run([sys.executable, os.path.join(REPO, 'tools', 'wm30', 'list.py'), '--json', lst],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=300)
    seen = {}
    try:
        with open(lst, encoding='utf-8') as f:
            for m in json.load(f)['matches']:
                seen.setdefault(m['era'], m['index'])
    except (OSError, ValueError, KeyError):
        seen = {0: 0}
    for era, idx in sorted(seen.items()):
        items.append({'id': 'wm30_e%d' % era, 'family': 'wm30', 'spec': '30 Years match %d (era %d)' % (idx, era),
                      'wm30': idx, 'entrances': 'full', 'hold_frames': MATCH_FRAMES,
                      'flags': ['--wwe2k14_wm30=%d' % idx], 'timeout_s': 2400})
    # modes and screens
    items.append({'id': 'mode_tour', 'family': 'mode', 'mode': 'tour', 'spec': 'navigator tour (nav_check default legs)',
                  'flags': [], 'timeout_s': 1500})
    with open(os.path.join(REPO, 'docs', 'systems', 'game_atlas.json'), encoding='utf-8') as f:
        nodes = list(json.load(f)['nodes'].keys())
    for k in range(0, len(nodes), 20):
        items.append({'id': 'mode_nodes_%02d' % (k // 20), 'family': 'mode', 'mode': 'nodes', 'nodes': nodes[k:k + 20],
                      'spec': 'atlas nodes %d-%d' % (k, min(k + 20, len(nodes)) - 1), 'flags': [], 'timeout_s': 1500})
    items.append({'id': 'mode_recording', 'family': 'mode', 'mode': 'recording',
                  'spec': "Jarrod's Session 91 recording (out\\s91\\jarrod_block.txt), normal speed", 'flags': [],
                  'timeout_s': 2700})
    items.append({'id': 'mode_cpu_finish', 'family': 'mode', 'mode': 'cpu_finish',
                  'spec': 'Daniel Bryan vs Brock Lesnar; players=0; pace=quick (to the finish) + pause menu',
                  'flags': [], 'timeout_s': 1800})
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, 'items.json'), 'w', encoding='utf-8') as f:
        json.dump(items, f, indent=1)
    cnt = {}
    for it in items:
        cnt[it['family']] = cnt.get(it['family'], 0) + 1
    print('%d items: %s -> %s' % (len(items), ', '.join('%s %d' % kv for kv in cnt.items()),
                                  os.path.join(OUT, 'items.json')))


if __name__ == '__main__':
    main()
