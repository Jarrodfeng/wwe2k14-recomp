"""Where a vtable starts: the ONE rule behind q slice's vt_start_of (qslice.py) and the field index's slot table (build_fields.build_types).
S111 FIELDSVT: the slot table read a vtable past the start of the next one (0x82023270 got 18 slots; it ends where 0x82023290 starts),
so a function was called "slot 16 of 0x82023270" that belongs to 0x82023290's table.

A vtable START is an address the program builds as a vtable pointer: a vptr store the field index saw (vstores), an RTTI vtable, or an
address a lis pair builds (the q index's imms) whose word is a function. A table's slots run from its start to the next start above it."""
import bisect


def index_starts(ix, is_func_word):
    """the starts the q index knows: its RTTI vtables, and every lis-pair immediate whose word is a function (a subobject's vptr is built this way)"""
    s = {v for (v,) in ix.execute('SELECT vt FROM rtti_vtable')}
    s |= {v for (v,) in ix.execute('SELECT DISTINCT value FROM imms WHERE value >= 0x82000000') if is_func_word(v)}
    return s


def next_start(vt, starts_sorted):
    """the first start strictly above vt, or None (starts_sorted: an ascending list)"""
    i = bisect.bisect_right(starts_sorted, vt)
    return starts_sorted[i] if i < len(starts_sorted) else None
