"""Convert the tip's db/seams rows (line-cited) to SEAMNOLINE's form with the merged qseam's own functions: each ours row that still
stores `line` is re-pointed at that line (set_anchor_at: anchor = the line's text, `occurrence` when several lines hold it), `ctx`
dropped; each topic file is written back with qseam's own writer. Run from the repo root after the merge."""
import json, os, sys
root = sys.argv[1]
sys.path.insert(0, os.path.join(root, 'tools', 'q'))
import qseam
changed = rows = 0
for topic in qseam.topics(root):
    path = qseam.entry_path(topic, root)
    entry = qseam.load(path)
    dirty = False
    for o in entry.get('ours', []):
        if 'line' in o:
            ln = o['line']
            st_text_ok = True
            try:
                qseam.set_anchor_at(o, ln, root)
            except Exception as e:  # the file moved: keep the anchor, drop the line, the check will say STALE
                print('could not re-point', topic, o.get('path'), ln, e)
                o.pop('line', None)
            o.pop('ctx', None)
            rows += 1
            dirty = True
        elif 'ctx' in o:
            o.pop('ctx', None); dirty = True
    if dirty:
        qseam._write(entry, root, False)
        changed += 1
print('topics rewritten', changed, 'rows', rows)
