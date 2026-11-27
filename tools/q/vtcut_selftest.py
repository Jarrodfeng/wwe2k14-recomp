"""Selftest of vtcut / build_fields.build_types: a vtable's slot table ends where the next vtable starts (S111 FIELDSVT).
A planted image holds two adjacent tables A (4 function words) and B (2) with nothing between. B is known only as a lis-pair immediate of the
q index (a subobject vptr): A must get 4 slots, not 6. Twin: B unknown to every index -> A runs on over B (6 slots, the old behaviour the cut removes).
Run: py -I tools\\q\\vtcut_selftest.py"""
import os
import sqlite3
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import build_fields as bf  # noqa: E402
import vtcut  # noqa: E402

A, B = 0x82000000, 0x82000010
FUNCS = [0x82000100 + 0x10 * i for i in range(6)]


def run(d, known_b, via):
    img = os.path.join(d, 'image.bin')
    with open(img, 'wb') as f:
        f.write(b''.join(struct.pack('>I', w) for w in FUNCS) + b'\0' * 64)
    ix = sqlite3.connect(os.path.join(d, 'index.db'))
    ix.executescript('drop table if exists rtti_vtable; drop table if exists imms;'
                     'create table rtti_vtable(vt integer primary key, col integer, td integer, nslots integer);'
                     'create table imms(func integer, site integer, value integer, op text);')
    if known_b and via == 'imms':
        ix.execute('insert into imms values (1, 2, ?, ?)', (B, 'lis'))
    if known_b and via == 'rtti':
        ix.execute('insert into rtti_vtable values (?,0,0,2)', (B,))
    ix.commit()
    ix.close()
    con = sqlite3.connect(':memory:')
    con.executescript('create table vstores(func integer, root integer, vt integer); create table calls(func integer, site integer, callee integer, argi integer, root integer);')
    con.execute('insert into vstores values (1, 1, ?)', (A,))
    if known_b and via == 'vstores':
        con.execute('insert into vstores values (2, 2, ?)', (B,))
    bf.build_types(con, set(FUNCS), img, {'a0': 1})
    return con.execute('select nslots from vtables where vt=?', (A,)).fetchone()[0]


def main():
    fails = []

    def check(ok, msg):
        print(('PASS ' if ok else 'FAIL ') + msg)
        if not ok:
            fails.append(msg)

    check(vtcut.next_start(5, [1, 5, 9, 12]) == 9 and vtcut.next_start(12, [1, 5, 9, 12]) is None, 'next_start: first start strictly above')
    with tempfile.TemporaryDirectory() as d:
        check(run(d, False, None) == 6, 'twin: the next table is known to no index -> no cut, 6 slots')
        for via in ('imms', 'rtti', 'vstores'):
            check(run(d, True, via) == 4, f'the next table known by {via} -> cut at its start, 4 slots')
    print('vtcut_selftest: %s' % ('FAIL (%d)' % len(fails) if fails else 'ok'))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
