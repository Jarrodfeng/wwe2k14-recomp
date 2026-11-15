"""S102 S0-6: a release zip (or a folder) must hold no pdb / map of the game exe. CLAUDE.md hard rule 1: wwe2k14.pdb names the recompiled game
code and stays on Jarrod's machines (the PC and the Surface kit).

  py -I tools\\release\\check_zip_symbols.py ZIP_OR_FOLDER [...]   exit 0 = clean, 1 = a symbol file is in it (listed), 2 = cannot read an input
  py -I tools\\release\\check_zip_symbols.py --selftest             the planted twins must be caught, the clean zip must pass

A member is a hit by NAME (.pdb / .map, any case, any folder) or by CONTENT (the first bytes are an MSF 7.00 pdb header, so a renamed pdb is
caught too). Called by tools\\release\\make_release.ps1 on the finished zip; the gate lane s102_symbols runs the selftest.
"""
import sys, zipfile, tempfile
from pathlib import Path

MAGICS = (b'Microsoft C/C++ MSF 7.00', b'Microsoft C/C++ program database')
BAD_EXT = ('.pdb', '.map')


def _hit(name, head):
    n = name.replace('\\', '/').lower()
    if n.endswith(BAD_EXT):
        return 'symbol file by name'
    if any(head.startswith(m) for m in MAGICS):
        return 'pdb by content'
    return None


def scan(path):
    p = Path(path)
    if not p.exists():
        raise FileNotFoundError(f'{path}: not found (a checker that reads nothing passes everything)')
    hits, n = [], 0
    if p.is_dir():
        for f in p.rglob('*'):
            if f.is_file():
                n += 1
                with open(f, 'rb') as fh:
                    h = _hit(f.name, fh.read(64))
                if h:
                    hits.append(f'{f.relative_to(p)}: {h}')
    else:
        with zipfile.ZipFile(p) as z:
            for i in z.infolist():
                if i.is_dir():
                    continue
                n += 1
                with z.open(i) as fh:
                    h = _hit(i.filename, fh.read(64))
                if h:
                    hits.append(f'{i.filename}: {h}')
    if n == 0:
        raise ValueError(f'{path}: holds no files (nothing was checked)')
    return n, hits


def selftest():
    fake_pdb = b'Microsoft C/C++ MSF 7.00\r\n\x1aDS\0\0\0' + b'\0' * 100
    d = Path(tempfile.mkdtemp(prefix='zipsym_'))

    def mk(name, members):
        with zipfile.ZipFile(d / name, 'w') as z:
            for k, v in members.items():
                z.writestr(k, v)
        return d / name

    clean = mk('clean.zip', {'r/wwe2k14.exe': b'MZ' + b'\0' * 50, 'r/README.md': b'hi'})
    twins = {
        'pdb_by_name': mk('t1.zip', {'r/wwe2k14.exe': b'MZ', 'r/wwe2k14.pdb': fake_pdb}),
        'pdb_upper_nested': mk('t2.zip', {'r/wwe2k14.exe': b'MZ', 'r/bin/WWE2K14.PDB': b'x'}),
        'pdb_renamed': mk('t3.zip', {'r/wwe2k14.exe': b'MZ', 'r/data/notes.txt': fake_pdb}),
        'map_file': mk('t4.zip', {'r/wwe2k14.exe': b'MZ', 'r/wwe2k14.map': b'x'}),
    }
    bad = 0
    n, hits = scan(clean)
    if hits:
        print('FAIL clean zip flagged:', hits)
        bad += 1
    else:
        print('ok   clean zip passes')
    for k, z in twins.items():
        n, hits = scan(z)
        if not hits:
            print(f'FAIL planted twin {k} NOT caught')
            bad += 1
        else:
            print(f'ok   planted twin {k} caught: {hits[0]}')
    try:
        scan(d / 'missing.zip')
        print('FAIL a missing input did not fail loudly')
        bad += 1
    except FileNotFoundError:
        print('ok   missing input fails loudly')
    mk('empty.zip', {})
    try:
        scan(d / 'empty.zip')
        print('FAIL an empty zip passed')
        bad += 1
    except ValueError:
        print('ok   empty zip fails loudly')
    for f in d.iterdir():
        f.unlink()
    d.rmdir()
    print('selftest', 'FAIL' if bad else 'PASS')
    return 1 if bad else 0


def main(argv):
    if argv[:1] == ['--selftest']:
        return selftest()
    if not argv:
        print(__doc__)
        return 2
    rc = 0
    for a in argv:
        try:
            n, hits = scan(a)
        except Exception as e:
            print(f'ERROR {e}')
            return 2
        for h in hits:
            print(f'SYMBOLS IN RELEASE: {a}: {h}')
        print(f'{a}: {n} files, {len(hits)} symbol file(s)')
        rc = rc or (1 if hits else 0)
    return rc


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
