r"""Self-test of slots_census.py on its committed expectation rows (no capture needed): the null cut passes, the cuts of the dead slots pass, and each plant is caught.
Plants: occupied_slot (cut slot 0 / slot 4 of Bryan's entrance), off_by_one (the dead slots 1-3 named one slot too high: 2-4 hits the read slot 4), stale_rule (Bryan's
slot choice applied to Brock's entrance, then Brock's choice applied to Bryan: the second breaks), match_cut (any cut in a match, where slot 0 is the only bake and is read)."""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import slots_census as sc  # noqa: E402

E = json.load(open(os.path.join(HERE, 'slots_expect.json')))
byname = {}
for k, v in E.items():
    n = len(v['slots'])
    kind = 'match' if n == 1 else ('bryan' if v['slots']['4']['reads'] else 'brock')
    byname[kind] = v
ok = True


def expect(label, got, want):
    global ok
    good = got == want
    ok &= good
    print('%-44s %s' % (label, 'ok' if good else 'FAIL got %r want %r' % (got, want)))


expect('null cut (nothing cut) on every capture', [sc.cut_violations(v, []) for v in byname.values()], [[]] * len(byname))
expect('bryan: cut 1,2,3 invisible', sc.cut_violations(byname['bryan'], [1, 2, 3]), [])
expect('brock: cut 1,2,3,4 invisible', sc.cut_violations(byname['brock'], [1, 2, 3, 4]), [])
expect('PLANT occupied_slot: bryan cut 0 caught', sc.cut_violations(byname['bryan'], [0]), [0])
expect('PLANT occupied_slot: bryan cut 4 caught', sc.cut_violations(byname['bryan'], [4]), [4])
expect('PLANT off_by_one: bryan cut 2,3,4 caught', sc.cut_violations(byname['bryan'], [2, 3, 4]), [4])
expect('PLANT stale_rule: brock rule (1-4) on bryan caught', sc.cut_violations(byname['bryan'], [1, 2, 3, 4]), [4])
expect('PLANT match_cut: cut 0 in a match caught', sc.cut_violations(byname['match'], [0]), [0])
sys.exit(0 if ok else 1)
