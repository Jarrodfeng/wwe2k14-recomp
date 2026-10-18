"""Annotate a flight recorder dump (logs/flight_NNN.txt, src/flight_recorder.h) with our function database names.

Every "sub_XXXXXXXX" in the dump that has an entry in db/*.json gets its proposed name appended, e.g.
"sub_XXXXXXXX+0x148" -> "sub_XXXXXXXX+0x148 [proposed_name (medium)]"; high-confidence names carry no tag. Names are our own annotations (db/), never game code.
The output goes next to the input as flight_NNN_named.txt unless --out is given; logs/ is not tracked, so a dump of
a real run never reaches the repo.

Usage:
  py tools/flight/flight_names.py out/build/win-amd64-release/logs/flight_213.txt [--out FILE] [--db DIR]
"""
import argparse
import glob
import json
import os
import re
import sys


def load_db(db_dir):
    names = {}
    for path in sorted(glob.glob(os.path.join(db_dir, '*.json'))):
        try:
            entries = json.load(open(path, encoding='utf-8'))
        except (OSError, ValueError) as e:
            print(f'skipping {path}: {e}', file=sys.stderr)
            continue
        if isinstance(entries, dict):
            entries = entries.get('functions') or entries.get('entries') or []
        for e in entries:
            if not isinstance(e, dict):
                continue
            addr, name = e.get('address'), e.get('name')
            if not addr or not name:
                continue
            conf = e.get('confidence')
            if conf and conf != 'high':
                name = f'{name} ({conf})'  # db confidence, so a low-confidence label isn't read as fact
            try:
                names.setdefault(int(str(addr), 16), name)
            except ValueError:
                pass
    return names


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument('dump')
    ap.add_argument('--out')
    ap.add_argument('--db', default=os.path.join(here, '..', '..', 'db'))
    args = ap.parse_args()
    names = load_db(args.db)
    text = open(args.dump, encoding='utf-8', errors='replace').read()
    hits = 0

    def repl(m):
        nonlocal hits
        name = names.get(int(m.group(1), 16))
        if not name:
            return m.group(0)
        hits += 1
        return f'{m.group(0)} [{name}]'

    out = re.sub(r'sub_([0-9A-Fa-f]{8})(?:\+0x[0-9A-Fa-f]+)?', repl, text)
    path = args.out or re.sub(r'\.txt$', '', args.dump) + '_named.txt'
    open(path, 'w', encoding='utf-8').write(out)
    print(f'{len(names)} db names, {hits} references named -> {path}')


if __name__ == '__main__':
    main()
