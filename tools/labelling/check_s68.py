"""Session 68: check a bulk workflow journal against the pack manifest before to_db.py merges it.

    py tools\\labelling\\check_s68.py JOURNAL [JOURNAL ...] [--gaps OUT.json]

Every settled address must belong to the run's packs (out\\harness\\labelling\\s68\\packs.json); lists addresses
of the packs that got no proper settled entry (placeholders, typos, missing) as gaps for a re-run, and the judge
tallies. Prints numbers and addresses only.
"""
import argparse
import collections
import json
import os
import re
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
PACKS = os.path.join(REPO, 'out', 'harness', 'labelling', 's68', 'packs.json')
ADDR = re.compile(r'^0x8[0-9A-F]{7}$')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('journals', nargs='+')
    ap.add_argument('--gaps')
    args = ap.parse_args()
    with open(PACKS, encoding='utf-8') as f:
        addresses = json.load(f)['addresses']
    owner = {a: pid for pid, al in addresses.items() for a in al}
    settled, judged, issues = {}, [], []
    for j in args.journals:
        with open(j, encoding='utf-8') as f:
            for line in f:
                o = json.loads(line)
                if o.get('type') != 'result':
                    continue
                v = o.get('result') or o.get('value')
                if isinstance(v, dict) and 'results' in v:
                    for r in v['results']:
                        a = str((r.get('settled') or {}).get('address', r.get('address', '')))
                        settled[a] = r
                elif isinstance(v, dict) and 'functions' in v:
                    judged += v['functions']
                    issues += v.get('systematic_issues') or []
    bad = [a for a in settled if not ADDR.match(a) or a not in owner]
    packs_seen = {owner[a] for a in settled if a in owner}
    want = {a for pid in packs_seen for a in addresses[pid]}
    gaps = sorted(want - {a for a in settled if a in owner})
    print(f'settled {len(settled)} from {len(packs_seen)} packs; bad addresses {len(bad)} {bad[:10]}; '
          f'gaps (pack functions without a settled entry) {len(gaps)} {gaps[:10]}')
    c = collections.Counter
    print(f'judged {len(judged)}: meaning {dict(c(f["S"].get("meaning") for f in judged))}, correct '
          f'{dict(c(f["S"].get("correct") for f in judged))}, invented {sum(1 for f in judged if f["S"].get("invented"))}')
    if args.gaps:
        with open(args.gaps, 'w', encoding='utf-8') as f:
            json.dump(gaps, f, indent=1)
        print('gaps ->', args.gaps)


if __name__ == '__main__':
    sys.exit(main())
