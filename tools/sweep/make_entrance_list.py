r"""Session 61: out\harness\sweep\lists\entrances.json - every wrestler of the roster list (make_lists.py) vs Daniel
Bryan (Bryan himself vs Brock Lesnar), with the route that ends just after the opponent's entrance starts
(scripts\entrance_vs_bryan.txt / entrance_vs_lesnar.txt). For the shader list: run with
run_sweep.ps1 -List entrances -KeepShaderStorage DIR.
"""
import json
import os

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LISTS = os.path.join(REPO, 'out', 'harness', 'sweep', 'lists')
SCRIPTS = os.path.join(REPO, 'tools', 'sweep', 'scripts')

roster = json.load(open(os.path.join(LISTS, 'roster.json')))
items = []
for it in roster['items']:
    cid = it['meta']['id']
    vs = 343 if cid == 324 else 324
    route = 'entrance_vs_lesnar.txt' if vs == 343 else 'entrance_vs_bryan.txt'
    items.append({'name': f'ent_{cid}', 'script': os.path.join(SCRIPTS, route),
                  'extra': f'--wwe2k14_launch=lesnar_bryan --wwe2k14_launch_participants={cid},{vs}',
                  'meta': {'id': cid, 'opponent': vs}})
out = os.path.join(LISTS, 'entrances.json')
json.dump({'list': 'entrances', 'items': items}, open(out, 'w'), indent=1)
print(len(items), 'items ->', out)
