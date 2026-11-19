"""Score a choice_predict.py output against harvested choice tables (S103 decomp experiment, Q4.3: whole CHOICE rows).

py -I tools\\harvest\\choice_score.py PRED.json TABLE.json [TABLE.json ...] [--show N]

Key = (file, mesh, look). For each key both sides have, compares the predicted choice with the table's choices (a table row may carry several
choices: the predicted one must equal the MOST-DRAWN one; it counts as 'seen' if it equals any). Fields: vs+ps (the shader pair), blend, alpha,
wmask. Names and numbers only; reads the player's tables locally.
"""
import json, sys, argparse
from collections import Counter

FIELDS = ('pair', 'blend', 'alpha', 'wmask')


def rows(doc):
    for m in doc.get('meshes', []):
        for look, lv in (m.get('looks') or {}).items():
            ch = lv.get('choices') or []
            if ch:
                yield (m.get('file'), m.get('mesh'), look), m, ch


def val(c, f):
    if f == 'pair':
        return (c.get('vs'), c.get('ps'))
    return c.get(f)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('pred')
    ap.add_argument('tables', nargs='+')
    ap.add_argument('--show', type=int, default=12)
    a = ap.parse_args()
    pred = {}
    for k, m, ch in rows(json.load(open(a.pred))):
        pred[k] = (m, ch[0])
    truth = {}
    for t in a.tables:
        for k, m, ch in rows(json.load(open(t))):
            best = max(ch, key=lambda c: c.get('n', 0))
            prev = truth.get(k)
            if prev is None or best.get('n', 0) > prev[1].get('n', 0):
                truth[k] = (m, best, ch)
    keys = sorted(set(pred) & set(truth), key=str)
    tot = Counter(); seen = Counter(); miss = {f: [] for f in FIELDS}
    for k in keys:
        pm, pc = pred[k]
        tm, best, ch = truth[k]
        for f in FIELDS:
            if val(pc, f) == val(best, f):
                tot[f] += 1
            else:
                miss[f].append((k, tm.get('effect'), val(pc, f), val(best, f)))
            if any(val(pc, f) == val(c, f) for c in ch):
                seen[f] += 1
    allok = sum(1 for k in keys if all(val(pred[k][1], f) == val(truth[k][1], f) for f in FIELDS))
    print('keys: predicted %d, in tables %d, joined %d (table keys not predicted %d)' % (len(pred), len(truth), len(keys), len(set(truth) - set(pred))))
    for f in FIELDS:
        print('%-6s %4d / %d match the most-drawn choice (%d equal some choice)' % (f, tot[f], len(keys), seen[f]))
    print('WHOLE  %4d / %d' % (allok, len(keys)))
    for f in FIELDS:
        c = Counter((e, str(p), str(t)) for _, e, p, t in miss[f])
        for (e, p, t), n in c.most_common(a.show):
            print('  MISS %-5s x%-3d %-16s predicted %s | game %s' % (f, n, e, p, t))


if __name__ == '__main__':
    main()
