"""2026-11-18: the finished release zip must hold the exes it should (the AVX2 exe wwe2k14_v3.exe is part of every release unless -NoV3Exe).

  py -I tools\\release\\check_zip_contents.py ZIP --require NAME [NAME ...]   exit 0 = all present, 1 = one is missing (listed), 2 = cannot read
  py -I tools\\release\\check_zip_contents.py --selftest                      a zip without wwe2k14_v3.exe must FAIL the check, a full one pass

Called by make_release.ps1 on the finished zip; the planted twin is in the selftest.
"""
import sys, tempfile, zipfile
from pathlib import Path


def top_level_missing(zip_path, required):
    """The required files must sit at the top level of the release folder (<name>/<file>), not just anywhere in the zip."""
    with zipfile.ZipFile(zip_path) as z:
        tops = {n.replace('\\', '/').rstrip('/') for n in z.namelist()}
    return [r for r in required if not any(t.count('/') == 1 and t.split('/')[1].lower() == r.lower() for t in tops)]


def v3_code_differs(zip_path):
    """wwe2k14_v3.exe must be a different build from wwe2k14.exe: their code-section hashes differ (tools\\release\\pe_text_hash.py)."""
    import importlib.util
    spec = importlib.util.spec_from_file_location('pe_text_hash', Path(__file__).with_name('pe_text_hash.py'))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    d = Path(tempfile.mkdtemp(prefix='zip_pe_'))
    hashes = []
    with zipfile.ZipFile(zip_path) as z:
        for r in ('wwe2k14.exe', 'wwe2k14_v3.exe'):
            member = next(n for n in z.namelist() if n.replace('\\', '/').count('/') == 1 and n.replace('\\', '/').split('/')[1].lower() == r)
            p = d / r
            p.write_bytes(z.read(member))
            hashes.append(mod.code_hash(str(p))[0])
    return hashes[0] != hashes[1]


def missing(zip_path, required):
    if not Path(zip_path).exists():
        raise FileNotFoundError(f'{zip_path}: not found')
    with zipfile.ZipFile(zip_path) as z:
        leaves = {n.replace('\\', '/').rstrip('/').split('/')[-1].lower() for n in z.namelist()}
    return [r for r in required if r.lower() not in leaves]


def selftest():
    d = tempfile.mkdtemp(prefix='zip_contents_')
    full, planted = Path(d, 'full.zip'), Path(d, 'no_v3.zip')
    for p, names in ((full, ('wwe2k14.exe', 'wwe2k14_v3.exe')), (planted, ('wwe2k14.exe',))):
        with zipfile.ZipFile(p, 'w') as z:
            for n in names:
                z.writestr('wwe2k14-recomp-vTEST/' + n, b'x')
    nested = Path(d, 'nested.zip')
    with zipfile.ZipFile(nested, 'w') as z:
        z.writestr('wwe2k14-recomp-vTEST/sub/wwe2k14.exe', b'x')
        z.writestr('wwe2k14-recomp-vTEST/sub/wwe2k14_v3.exe', b'x')
    ok = top_level_missing(nested, ('wwe2k14.exe', 'wwe2k14_v3.exe')) == ['wwe2k14.exe', 'wwe2k14_v3.exe'] and not top_level_missing(full, ('wwe2k14.exe', 'wwe2k14_v3.exe')) and not missing(full, ('wwe2k14.exe', 'wwe2k14_v3.exe')) and missing(planted, ('wwe2k14.exe', 'wwe2k14_v3.exe')) == ['wwe2k14_v3.exe']
    print('check_zip_contents selftest:', 'PASS (the zip without wwe2k14_v3.exe is caught)' if ok else 'FAIL')
    return 0 if ok else 1


def main():
    a = sys.argv[1:]
    if a == ['--selftest']:
        return selftest()
    if len(a) < 3 or a[1] != '--require':
        print(__doc__)
        return 2
    try:
        miss = missing(a[0], a[2:])
    except (OSError, zipfile.BadZipFile) as e:
        print('check_zip_contents: cannot read', e)
        return 2
    miss = miss + top_level_missing(a[0], [r for r in a[2:] if r not in miss])
    if not miss and 'wwe2k14_v3.exe' in a[2:] and not v3_code_differs(a[0]):
        print('check_zip_contents: FAIL wwe2k14_v3.exe has the same code as wwe2k14.exe (the wide build did not happen)')
        return 1
    for m in miss:
        print(f'check_zip_contents: FAIL the zip has no {m}')
    if not miss:
        print('check_zip_contents: ok (' + ', '.join(a[2:]) + ')')
    return 1 if miss else 0


if __name__ == '__main__':
    sys.exit(main())