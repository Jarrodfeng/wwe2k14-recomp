"""Session 30 (#7): db/ids/match_types.json from a menu crawl (tools/monkey/crawl.py map.json).

  py tools/monkey/match_types.py out/harness/crawl/<stamp>_play/map.json [--launched 0,3,...] [--out db/ids/match_types.json]

One entry per match-type id the PLAY menus lead to: the id (the setup screen's menu id - 2000), the menu path(s) to it,
and the evidence. --launched lists the ids a Quick Match (--wwe2k14_launch=@quick --wwe2k14_quick_type=T) loaded in
a run (match clock running, screenshot consistent): those become "launched"; --loaded = ran, look not confirmed. Names are the menus' own labels (public match names, as the arena and
character tables do).
"""
import argparse
import json
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CAPTURED = {0: 'lesnar_bryan', 3: 'tag_hellno_shield', 23: 'rumble_40', 39: 'chamber_6man', 49: 'hiac_cena_brodus'}
RULE_TYPES = {81: 'Inferno', 26: 'brawl', 27: 'brawl', 28: 'brawl', 29: 'brawl', 30: 'brawl', 31: 'brawl'}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('map')
    ap.add_argument('--launched', default='', help='type ids a Quick Match loaded (comma list)')
    ap.add_argument('--launch-runs', default='', help='the runs of those launches (text for the evidence)')
    ap.add_argument('--loaded', default='', help='type ids a Quick Match ran but whose look did not confirm the type')
    ap.add_argument('--loaded-note', default='', help='what the screenshots showed for --loaded')
    ap.add_argument('--out', default=os.path.join(REPO, 'db', 'ids', 'match_types.json'))
    a = ap.parse_args()
    m = json.load(open(a.map, encoding='utf-8'))
    launched = {int(x) for x in a.launched.split(',') if x.strip()}
    loaded = {int(x) for x in a.loaded.split(',') if x.strip()}
    types = {}
    for e in m['entries']:
        if e.get('type') is None:
            continue
        t = e['type']
        path = '%s > %s' % (e['path'], e['label'])
        rec = types.setdefault(t, {'id': t, 'name': e['label'], 'menu_paths': [], 'setup_menu': e['menu'],
                                   'evidence': [], 'confidence': 'medium'})
        rec['menu_paths'].append(path)
    for t, rec in types.items():
        rec['evidence'].append('crawl: the setup screen after %s has menu id %d = 2000 + %d (run %s, tools/monkey/'
                               'crawl.py, Session 30)' % (rec['menu_paths'][0], rec['setup_menu'], t, m.get('run')))
        if t in CAPTURED:
            rec['evidence'].append('capture %s: M+8 = %d (docs/systems/match_setup.md)' % (CAPTURED[t], t))
            rec['confidence'] = 'high'
        if t in RULE_TYPES:
            rec['evidence'].append('docs/systems/match_setup.md: rule match type %d = %s' % (t, RULE_TYPES[t]))
        if t in launched:
            rec['evidence'].append('launched: a Quick Match with --wwe2k14_quick_type=%d ran (match clock > 10 s) and the '
                                   'screenshot matches the type%s' % (t, ' (runs %s)' % a.launch_runs if a.launch_runs else ''))
            rec['confidence'] = 'high'
        if t in loaded:
            rec['evidence'].append('a Quick Match with --wwe2k14_quick_type=%d ran (match clock > 10 s); %s' % (t, a.loaded_note))
    doc = {
        'about': 'WWE 2K14 (Xbox 360) match-type ids: the value the type setter sub_822CA9C0 stores at M+8 '
                 '(docs/systems/match_setup.md) and --wwe2k14_quick_type takes. Found by the Session 30 menu crawler: '
                 'every PLAY entry leads to a setup screen whose menu id is 2000 + the type; checked against the five '
                 'types known from match captures (0, 3, 23, 39, 49) and the rule types in match_setup.md (81 '
                 'Inferno, 26-31 brawl). The type table holds 119 records: ids missing here are not reached from PLAY '
                 '(e.g. 1, 2, 37 from Session 27, other modes). confidence high = captured or launched, medium = menu '
                 'id only.',
        'source': 'tools/monkey/match_types.py from %s' % os.path.relpath(a.map, REPO).replace('\\', '/'),
        'entries': [types[t] for t in sorted(types)],
    }
    with open(a.out, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(doc, f, indent=2)
        f.write('\n')
    print('%s: %d match types (%d high)' % (a.out, len(types), sum(1 for r in types.values() if r['confidence'] == 'high')))
    return 0


if __name__ == '__main__':
    sys.exit(main())
