r"""qfield_strides: the STRIDE rows of `q field OFF` (S111 QFIX4). The field index models no indexed access (lwzx / mulli): an address base + K + stride*index
(SmartMenu: [SM + 1388 + 5292*ctx + 4]) is not in its rows, so `q field 1392` missed the context readers. `q struct` (STRUCTREC, tools\q\qstruct.py) does
name them: one row per (base K, stride) with the readers / writers. This module keeps those rows where `q field` can find them:

  * a store: out/harness/index/field_strides.json, {target label: {'target': text, 'rows': [stride row of q struct]}}, written by every `q struct` run
    (qstruct.main calls record) and by `q field OFF --strides TARGET ...` (which runs q struct on the target itself, ~10 s the first time);
  * lookup(OFF): the rows with a stride whose base K == OFF, or whose base K reduced mod a stride == OFF (a base written as one element's offset plus a
    whole stride: +6684 + 5292*k is the field at 1392); `q field 1392` finds +1392 + 5292*k; each row is printed with its stride.
Only the rows of classes `q struct` has been run on are known: the answer says how many classes it looked in, never that nothing else exists.
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)


def store_path():
    import build_fields as bf
    _gen, _image, db = bf.default_paths(None, None)
    return os.path.join(os.path.dirname(db), 'field_strides.json')


def load(path=None):
    try:
        with open(path or store_path(), encoding='utf-8') as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


KEEP = ('off', 'stride', 'widths', 'kinds', 'reads', 'writes', 'passed', 'name', 'index', 'readers', 'writers')


def record(label, target, rows, path=None):
    """store the stride rows (a row with a stride) of one `q struct` answer; an answer with none removes the entry"""
    p = path or store_path()
    d = load(p)
    keep = [{k: r[k] for k in KEEP if k in r} for r in rows if r.get('stride')]
    if keep:
        d[label] = {'target': target, 'rows': keep}
    else:
        d.pop(label, None)
    try:
        os.makedirs(os.path.dirname(p), exist_ok=True)
        tmp = p + '.tmp%d' % os.getpid()
        with open(tmp, 'w', encoding='utf-8') as f:
            json.dump(d, f, indent=0)
        os.replace(tmp, p)
    except OSError:
        pass      # a read-only index folder: the answer of this run is still right, it is just not kept


def matches(row, off):
    """-> 'K' (the base is OFF), 'K mod stride' (OFF is the base reduced by one of its strides), or None"""
    c, sts = row['off'], row['stride']
    if c == off:
        return 'K'
    if any(g and c % g == off and c != off for g in sts):
        return 'K mod stride'
    return None


def lookup(off, path=None, d=None):
    """-> {'classes': N classes in the store, 'rows': [{'class', 'target', 'match', ...row}]}"""
    d = load(path) if d is None else d
    out = []
    for label, e in sorted(d.items()):
        for r in e['rows']:
            m = matches(r, off)
            if m:
                out.append(dict(r, **{'class': label, 'target': e['target'], 'match': m}))
    return {'classes': len(d), 'rows': out}


def scan(targets, con, quiet=False):
    """run q struct's engine on each target and record its stride rows; -> [label]"""
    import qslice
    import qstruct
    src = qslice.QSource(con)
    labels = []
    for t in targets:
        tg, rows, _al, _sc = qstruct.struct_of(src, t, qstruct._resolver(con))
        record(tg.label, t, rows)
        labels.append(tg.label)
    return labels


def print_rows(res, out=print):
    if not res['rows']:
        return
    out(f"stride rows (base + K + stride*index; the field index models no indexed access; from `q struct`, {res['classes']} classes in the store):")
    for r in res['rows']:
        st = ' + '.join(f'{g}*k{i + 1}' if len(r['stride']) > 1 else f'{g}*k' for i, g in enumerate(r['stride']))
        mod = f"  [OFF is K mod {r['stride'][0]}]" if r['match'] == 'K mod stride' else ''
        out(f"  {r['class']}: +{r['off']} + {st}  (stride {'/'.join(map(str, r['stride']))}){mod}  w{'/'.join(map(str, r['widths'])) or '-'}  "
            f"R {r['reads']} W {r['writes']}  {r.get('name', '')}")
        if r['readers']:
            out('      R ' + ', '.join(r['readers'][:6]) + (f" +{len(r['readers']) - 6}" if len(r['readers']) > 6 else ''))
        if r['writers']:
            out('      W ' + ', '.join(r['writers'][:6]) + (f" +{len(r['writers']) - 6}" if len(r['writers']) > 6 else ''))
        if r.get('index'):
            out('      index ' + ', '.join(r['index'][:2]))


def note(res, off):
    """the line printed when nothing matched: what was and was not searched"""
    return (f"(stride rows: {res['classes']} classes in the store; none holds +{off}. Only classes `q struct` has run on are known: "
            f"q field {off} --strides TARGET scans one (a vtable, sub_X, g:ADDR))")
