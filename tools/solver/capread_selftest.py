r"""Selftest for tools\solver\capread.py on a small synthetic frame file (our own bytes, no game data).
Covers the bank-def layouts: full bank, a base chain, a patch, and the c2d-capture shapes that crashed the reader
(base >= def index = zero bank, patch range past the table = ignored; capture_reader.h Resolve gives the same).
Before the fix def 2 raised IndexError (the S103 c2d_pause failure), so this selftest fails on the old reader.
"""
import struct
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import capread  # noqa: E402


def build():
    full = np.zeros((2, 1024), '<u4')
    full[0, :] = 7
    full[1, :] = 9
    # defs: 0 = full 0; 1 = base 0 + patch block 3; 2 = base 99 (past the table); 3 = base 3 (itself); 4 = full 1 with patches past the table
    defs = np.array([(capread.NO_BASE, 0, 0, 0), (0, 0, 0, 1), (99, 0, 0, 0), (3, 0, 0, 0), (capread.NO_BASE, 1, 5, 2)], capread.BANKDEF_DT)
    patches = np.zeros(1, capread.BANKPATCH_DT)
    patches[0]['block'] = 3
    patches[0]['dwords'] = np.arange(100, 116)
    secs = [(capread.SEC['constant_banks'], 4096, len(full), full.tobytes()),
            (capread.SEC['bank_defs'], 16, len(defs), defs.tobytes()),
            (capread.SEC['bank_patches'], 68, len(patches), patches.tobytes())]
    off = 32 + 24 * len(secs)
    head = struct.pack('<IIIIQII', 0x52464352, 1, 5, len(secs), 6, 0, 0)
    table, body = b'', b''
    for sid, es, cnt, data in secs:
        table += struct.pack('<IIQQ', sid, es, cnt, off + len(body))
        body += data
    return head + table + body


def main():
    fr = capread.Frame(build())
    b = fr.banks.view('<u4')
    ok = True

    def check(name, cond):
        nonlocal ok
        print(('ok   ' if cond else 'FAIL ') + name)
        ok &= bool(cond)
    check('def 0 is full bank 0', (b[0] == 7).all())
    check('def 1 = def 0 + patch block 3', (b[1, :48] == 7).all() and (b[1, 48:64] == np.arange(100, 116)).all() and (b[1, 64:] == 7).all())
    check('def 2 (base past the table) is a zero bank', (b[2] == 0).all())
    check('def 3 (base = itself) is a zero bank', (b[3] == 0).all())
    check('def 4 (full 1, patches past the table) is full bank 1', (b[4] == 9).all())
    print('PASS' if ok else 'FAIL')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
