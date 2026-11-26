#!/usr/bin/env python3
r"""The work list of the second reading pass (S110): every unit whose NEWEST reading says "listing needed", with a FRESH fact
sheet each (so the sheets carry the newest observed callers and pattern tags), split into N list files for N parallel
`qlsheet.py` runs.

  py -I tools\q\qlsheet_todo.py OUT_DIR [--parts N] [--skip-model NAME]

Writes OUT_DIR\sheets\UNIT.txt and OUT_DIR\part_K.lst (sheet paths, K = 1..N, default 6). --skip-model leaves out units whose
newest reading already came from that model (default opus-low-listing: read with a listing once already).
OUT_DIR must be under out\ .
"""
import os
import sqlite3
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qread  # noqa: E402
import qsheet  # noqa: E402


def main(argv):
    args = [a for i, a in enumerate(argv) if not a.startswith('--') and (i == 0 or argv[i - 1] not in ('--parts', '--skip-model'))]
    if len(args) != 1:
        print(__doc__)
        return 2
    out = os.path.abspath(args[0])
    if (os.sep + 'out' + os.sep) not in os.path.normcase(out) + os.sep:
        print('refused: OUT_DIR must be under out\\')
        return 1
    parts = int(argv[argv.index('--parts') + 1]) if '--parts' in argv else 6
    skip_model = argv[argv.index('--skip-model') + 1] if '--skip-model' in argv else 'opus-low-listing'
    c = sqlite3.connect('file:%s?mode=ro' % qread.READ_DB.replace('\\', '/'), uri=True)
    last = {}
    for u, ls, model in c.execute('select unit_id,listing_needed,model from unit_read order by id'):
        last[u] = (ls, model)
    c.close()
    todo = sorted(u for u, (ls, model) in last.items() if ls and model != skip_model)
    os.makedirs(os.path.join(out, 'sheets'), exist_ok=True)
    paths, total = [], 0
    for u in todo:
        text = qsheet.sheet(u, earlier=False)       # S111 FOLD: the reader is not shown the unit's earlier reading or names
        p = os.path.join(out, 'sheets', '%d.txt' % u)
        with open(p, 'w', encoding='utf-8', newline='\n') as f:
            f.write(text if text.endswith('\n') else text + '\n')
        paths.append(p)
        total += len(text.encode('utf-8'))
    for k in range(parts):
        with open(os.path.join(out, 'part_%d.lst' % (k + 1)), 'w', encoding='utf-8', newline='\n') as f:
            f.write('\n'.join(paths[k::parts]) + '\n')
    print('%d units need the listing (newest reading; %s readings left out), %d bytes of sheets, %d part lists -> %s'
          % (len(todo), skip_model, total, parts, out))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
