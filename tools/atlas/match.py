"""Session 91 (the game atlas, step 4): build any match from names - the match builder's front end.

  py tools/atlas/match.py "Daniel Bryan vs Brock Lesnar; type=one on one > normal; arena=over the limit; pace=quick"
                          [--name main] [--dry] [--compare NAME] [--wait]
  py tools/atlas/match.py "Cena, Orton, Bryan, Punk, Sheamus, Del Rio; type=elimination chamber > normal"
  py tools/atlas/match.py "royal rumble; type=royal rumble > 30; rumble=30 random"
MCP: game_match SPEC (tools/live/mcp_server.py).

Spec: parts separated by ';'. The part without '=' lists the wrestlers (', ' or ' vs '), in seat order. Keys:
  type=    an atlas path name ("one on one > ladder", "royal rumble > 30") or a match type number (db/ids/match_types)
  arena=   an arena name (db/ids/arenas.json) or a BG number; default: the game's own (random) pick
  pace=    normal | quick | epic
  players= how many of the first wrestlers get controllers 1, 2, ... (default 1; 0 = all CPU)
  rumble=  N random | names, ... (Royal Rumble entrants; finalize draws the order)
  title=   T0/T1 title ids per side (numbers; 255 = none)
  o=       K:V,... raw option-block bytes (R+8+K; MATCH RULES rows, readers c / c_review)
Names: closest match in db/ids (an unknown or ambiguous name is an error that lists the candidates).
The game side (src/scenario.h, --wwe2k14_match / --wwe2k14_match_go) builds the match through the menus' own path
(menu state S, the type's defaults, finalize) at the idle main menu; this tool first takes the game there with the
navigator (tools/atlas/nav.py).
"""
import argparse
import difflib
import json
import os
import random
import re
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, 'tools', 'atlas'))
import nav  # noqa: E402

IDS = os.path.join(REPO, 'db', 'ids')
PACE = {'normal': 0, 'quick': 1, 'epic': 2}


class SpecError(Exception):
    pass


def load(name):
    with open(os.path.join(IDS, name), encoding='utf-8') as f:
        return json.load(f)['entries']


def lookup(words, what='wrestler', limit=20):
    """Candidates for an MCP caller to choose from: [(id, name)] whose names contain every word (any order)."""
    entries = load('arenas.json') if what == 'arena' else load('characters.json')
    want = set(re.findall(r'[a-z0-9]+', words.lower()))
    hits = [(e['id'], e['name']) for e in entries if e.get('name') and
            want <= set(re.findall(r'[a-z0-9]+', e['name'].lower()))]
    if not hits:
        low = {e['name'].lower(): e for e in entries if e.get('name')}
        hits = [(low[k]['id'], low[k]['name']) for k in difflib.get_close_matches(words.lower(), list(low), n=limit,
                                                                                    cutoff=0.4)]
    return hits[:limit]


def pick(name, entries, what, key='name'):
    name = name.strip()
    m = re.search(r'#(\d+)', name)  # "#356", "Name (#356)" = that id, as given (a pick from lookup / a candidate list)
    if m:
        return int(m.group(1))
    if re.fullmatch(r'\d+', name):
        return int(name)
    low = {e[key].lower(): e for e in entries if e.get(key)}
    if name.lower() in low:
        return low[name.lower()]['id']
    # Words in any order ("Retro Cena" = John Cena (Retro)); the name with the fewest extra words wins, so "Cena" is
    # John Cena and "Cena Retro" the retro one. A tie is ambiguous.
    words = lambda s: set(re.findall(r'[a-z0-9]+', s.lower()))
    want = words(name)
    sub = sorted(((len(words(k) - want), e) for k, e in low.items() if want <= words(k)), key=lambda t: t[0])
    if sub and (len(sub) == 1 or sub[0][0] < sub[1][0]):
        return sub[0][1]['id']
    sub = [e for _, e in sub]
    close = difflib.get_close_matches(name.lower(), list(low), n=6, cutoff=0.5)
    cands = ['%s (#%d)' % (e[key], e['id']) for e in sub][:8] or ['%s (#%d)' % (low[k][key], low[k]['id']) for k in close]
    raise SpecError('%s "%s": %s' % (what, name, ('did you mean: ' + ', '.join(cands) + ' - give the #id to pick one')
                                     if cands else 'not found'))


def build_spec(text, atlas=None):
    """-> (the game's numeric spec string, a readable summary)."""
    chars = [e for e in load('characters.json') if e.get('kind', 'wrestler') in ('wrestler', 'diva', None)]
    arenas = load('arenas.json')
    out, summary = {}, []
    names, players = [], 1
    for part in [p.strip() for p in text.split(';') if p.strip()]:
        if '=' not in part:
            names = [n for n in re.split(r'\s*,\s*|\s+vs\.?\s+', part, flags=re.I) if n and n.lower() != 'royal rumble']
            continue
        k, v = [x.strip() for x in part.split('=', 1)]
        k = k.lower()
        if k == 'type':
            if re.fullmatch(r'\d+', v):
                out['type'] = int(v)
            else:
                atlas = atlas or nav.Atlas()
                hits = [h for h in atlas.resolve(v) if h.startswith('setup:')]
                if len(hits) != 1:
                    raise SpecError('type "%s": %s' % (v, ', '.join(atlas.names.get(h, h) for h in
                                                                    atlas.resolve(v)[:10]) or 'no setup screen'))
                out['type'] = int(hits[0].split(':')[1])
                summary.append(atlas.names[hits[0]])
        elif k == 'arena':
            out['arena'] = pick(v, arenas, 'arena')
        elif k == 'pace':
            if v.lower() not in PACE:
                raise SpecError('pace: normal | quick | epic')
            out['pace'] = PACE[v.lower()]
        elif k == 'players':
            players = int(v)
        elif k == 'title':
            out['title'] = v
        elif k == 'o':
            out['o'] = v
        elif k == 'rumble':
            m = re.fullmatch(r'(\d+)\s*random', v, re.I)
            if m:
                pool = [e['id'] for e in chars if e.get('confidence') in ('high', 'medium') and e['id'] < 400]
                ids = random.sample(pool, int(m.group(1)))
            else:
                ids = [pick(n, chars, 'wrestler') for n in v.split(',') if n.strip()]
            out['rumble'] = ','.join(str(i) for i in ids)
        else:
            raise SpecError('unknown key "%s"' % k)
    if 'type' not in out:
        if len(names) == 2:
            out['type'] = 0
        else:
            raise SpecError('type= is needed for %d wrestlers' % len(names))
    ids = [pick(n, chars, 'wrestler') for n in names]
    if ids:
        out['p'] = ','.join('%d:%d' % (i, s if s < players else -1) for s, i in enumerate(ids))
    byid = {e['id']: e['name'] for e in chars}
    summary.append(' vs '.join(byid.get(i, str(i)) for i in ids))
    spec = ';'.join('%s=%s' % (k, out[k]) for k in ('type', 'p', 'arena', 'pace', 'o', 'title', 'rumble') if k in out)
    return spec, '; '.join(s for s in summary if s)


def run(spec, name='main', compare='', wait=True, say=print):
    n = nav.Nav(name)
    for line in n.go('list:1'):
        say(line)
    go = int(time.time()) % 1000000 + 1
    lines = ['cvar wwe2k14_match %s' % spec, 'cvar wwe2k14_match_compare %s' % (compare or '""'),
             'cvar wwe2k14_match_go %d' % go]
    if wait:
        lines += ['wait_state seq 9 3600 2', 'log_state seq']
    st, _, failed = n.batch(lines, timeout=900)
    return failed, st


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('spec')
    ap.add_argument('--name', default='main')
    ap.add_argument('--dry', action='store_true', help='print the numeric spec only')
    ap.add_argument('--compare', default='', help='menu capture NAME to byte-compare against')
    ap.add_argument('--no-wait', action='store_true')
    a = ap.parse_args()
    try:
        spec, summary = build_spec(a.spec)
    except SpecError as x:
        print('SPEC: %s' % x)
        return 2
    print('spec: %s  (%s)' % (spec, summary))
    if a.dry:
        return 0
    failed, st = run(spec, a.name, a.compare, not a.no_wait)
    print('in game (sequence %s)' % st.get('seq') if not failed else 'FAILED: %s' % failed)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
