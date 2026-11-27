r"""qwhen_slots: the VTABLE SLOT step of `q when` (S111 QFIX4). A function that no call edge reaches but a vtable word points at (a sequence's update, a class's
slot call) is a root of its own kind: `slot`. This module answers "which vtable / slot / class / constructor / sequence id holds this function":

  * the data words pointing at the function: the index's `ptrs` (`q vtable --of`), each placed in its table by qvtable.of_func's bounds (the `tables` catalog,
    else the RTTI vtable; a run-start guess is NOT trusted here and is dropped, so a stray data word never becomes a root);
  * the vtable's class: its RTTI class name (index.db rtti_class), the constructors the field index records storing it (`ctors`, `vstores`),
  * the sequence id: `q seqids` (CALLARGS: the r4 each ctor passes to the base sequence ctor sub_82C3A298), joined by the vtable.

Used by qwhen.locate when the ordinary climb (task / phase / node / side walk) finds no root. Never read the game; computed from the indexes.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

TRUSTED = ('table', 'rtti')           # of_func's table_by values that start with these are bounded by a real table


class _LazyImg:
    """of_func reads the image only for its run-start heuristic; this reads it on first use only (and the heuristic rows are dropped anyway)"""

    def __init__(self):
        self._i = None

    def word(self, a):
        if self._i is None:
            import qvtable
            import q as qm
            self._i = qvtable.Img(os.path.join(os.path.dirname(qm.DB), 'image.bin'))
        return self._i.word(a)


def make_slots(con):
    """-> slots(func) -> [{'vt', 'slot', 'class', 'ctors': [addr], 'seq': [id]}] (empty = no table holds it). One function per call is cheap (an index point query)."""
    import qvtable
    img = _LazyImg()
    memo = {}
    state = {}

    def seq_by_vt():
        if 'seq' not in state:
            state['seq'], state['fdb'] = {}, None
            try:
                import qseqids
                import qslice
                src = qslice.QSource(con)
                state['fdb'] = src.fdb() or None
                for r in qseqids.seqids(src, con=con):
                    for v in r['vtables'][-1:]:      # the last vtable a ctor stores is its own class (an earlier one a base class's)
                        state['seq'].setdefault(v, set()).update(r['ids'])
            except Exception:      # noqa: BLE001 - no CALLARGS / no field index: the slot is still named, without a sequence id
                pass
        return state['seq']

    def ctors_of_vt(vt):
        seq_by_vt()
        db = state['fdb']
        if not db:
            return []
        try:
            return sorted({r[0] for r in db.execute('SELECT func FROM ctors WHERE vt = ?', (vt,))})
        except Exception:      # noqa: BLE001
            return []

    def slots(a):
        if a in memo:
            return memo[a]
        out = []
        for p in qvtable.of_func(con, img, a)['points']:
            if not p['table_by'].startswith(TRUSTED):
                continue
            vt = int(p['vtable'], 16)
            out.append({'vt': vt, 'slot': p['slot'], 'class': p['class'], 'ctors': ctors_of_vt(vt)[:4], 'seq': sorted(seq_by_vt().get(vt, ()))})
        memo[a] = out
        return out

    return slots


def text(fn, s):
    """the words of one slot row"""
    cls = f" (class {s['class']})" if s.get('class') else ''
    ct = f", vptr stored by {', '.join(fn(c) for c in s['ctors'])}" if s.get('ctors') else ''
    sq = f", sequence id {'/'.join(str(i) for i in s['seq'])} (q seqids)" if s.get('seq') else ''
    return f"slot {s['slot']} of vtable 0x{s['vt']:08X}{cls}{ct}{sq}"
