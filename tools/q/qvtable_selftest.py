"""Selftest for `q vtable` (S103 lane QV). 0 game runs.

A) synthetic fixture (invented table, in-memory index): the table ends at the first non-function word, --of places the
   word in its run, slot indexes are counted from the run start.
B) real data (needs out/harness/index/index.db + image.bin, else SKIP): the probes' hand-worked answers
   slot 2 of 0x8206AF90 = sub_82BD3AB8; slot 15 of 0x8206C7A0 = sub_827C17B8; slot 2 of 0x8206C7A0 = sub_82C27620;
   `--of sub_82BD3AB8` lists 0x8206AF90 slot 2.
C) planted wrong-slot twins must be REJECTED by the same checker (a claim with the wrong slot / table).
Exit 1 = a check failed, 2 = a planted twin was not caught.
"""
import os
import sqlite3
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qvtable  # noqa: E402


def fixture():
    con = sqlite3.connect(':memory:')
    con.row_factory = sqlite3.Row
    con.executescript('''CREATE TABLE funcs(addr INTEGER PRIMARY KEY, name TEXT);
        CREATE TABLE ptrs(at INTEGER, value INTEGER, kind TEXT);
        CREATE TABLE rtti_class(td INTEGER PRIMARY KEY, mangled TEXT, name TEXT);
        CREATE TABLE rtti_vtable(vt INTEGER PRIMARY KEY, col INTEGER, td INTEGER, nslots INTEGER);''')
    fs = [0x82100000, 0x82100100, 0x82100200, 0x82100300]
    con.executemany('INSERT INTO funcs VALUES(?,?)', [(a, f'sub_{a:08X}') for a in fs])
    base = 0x82000100
    words = [0x11111111, fs[0], fs[1], fs[2], fs[1], 0x00000007, fs[3], fs[3]]     # junk, table of 4, junk, table of 2
    data = bytearray(0x200)
    for i, w in enumerate(words):
        data[0x100 + 4 * i:0x104 + 4 * i] = struct.pack('>I', w)
    img = qvtable.Img.__new__(qvtable.Img)
    img.data = bytes(data)
    for i, w in enumerate(words):
        if w in fs:
            con.execute('INSERT INTO ptrs VALUES(?,?,?)', (base + 4 * i, w, 'func'))
    return con, img, base, fs


def claim_slot(con, img, base, slot, target):
    r = qvtable.slots_of(con, img, base, 64, slot, False)
    return r['slots'][0]['target'] == target


def claim_of(con, img, addr, vt, slot):
    r = qvtable.of_func(con, img, addr)
    return any(p['vtable'] == f'0x{vt:08X}' and p['slot'] == slot for p in r['points'])


def main():
    fails, missed = [], []

    def ok(name, cond):
        print(('ok   ' if cond else 'FAIL ') + name)
        if not cond:
            fails.append(name)

    def twin(name, cond):
        print(('ok   (rejected) ' if not cond else 'MISS (accepted) ') + name)
        if cond:
            missed.append(name)

    con, img, base, fs = fixture()
    t = base + 4
    ok('A table ends at the first non-function word', qvtable.slots_of(con, img, t, 64, None, False)['table_slots'] == 4)
    ok('A slot 2 = third function', claim_slot(con, img, t, 2, 'sub_82100200'))
    ok('A --of places the word in its run', claim_of(con, img, fs[2], t, 2))
    ok('A second run counted from its own start', claim_of(con, img, fs[3], base + 24, 1))
    twin('A wrong slot (slot 1 claimed = third function)', claim_slot(con, img, t, 1, 'sub_82100200'))
    twin('A wrong table in --of (slot 3)', claim_of(con, img, fs[2], t, 3))

    real = os.path.join(os.path.dirname(HERE), '..', 'out', 'harness', 'index')
    try:
        import q as qm
        rc = qm.connect(qm.DB)
        rimg = qvtable.Img(os.path.join(os.path.dirname(qm.DB), 'image.bin'))
        rc.execute('SELECT 1 FROM funcs LIMIT 1')
    except Exception as e:      # noqa: BLE001
        print(f'SKIP B/C real data ({e})')
        rc = None
    if rc is not None:
        ok('B slot 2 of 0x8206AF90 = sub_82BD3AB8', claim_slot(rc, rimg, 0x8206AF90, 2, 'sub_82BD3AB8'))
        ok('B slot 15 of 0x8206C7A0 = sub_827C17B8', claim_slot(rc, rimg, 0x8206C7A0, 15, 'sub_827C17B8'))
        ok('B slot 2 of 0x8206C7A0 = sub_82C27620', claim_slot(rc, rimg, 0x8206C7A0, 2, 'sub_82C27620'))
        ok('B --of sub_82BD3AB8 lists 0x8206AF90 slot 2', claim_of(rc, rimg, 0x82BD3AB8, 0x8206AF90, 2))
        twin('C slot 3 of 0x8206AF90 = sub_82BD3AB8 (wrong-slot twin)', claim_slot(rc, rimg, 0x8206AF90, 3, 'sub_82BD3AB8'))
        twin('C slot 2 of 0x8206C7A0 = sub_827C17B8 (wrong table twin)', claim_slot(rc, rimg, 0x8206C7A0, 2, 'sub_827C17B8'))
        twin('C --of lists 0x8206AF90 slot 3 (wrong-slot twin)', claim_of(rc, rimg, 0x82BD3AB8, 0x8206AF90, 3))
    if fails:
        print(f'{len(fails)} check(s) failed')
        return 1
    if missed:
        print(f'{len(missed)} planted twin(s) not caught')
        return 2
    print('qvtable selftest: all ok')
    return 0


if __name__ == '__main__':
    sys.exit(main())
