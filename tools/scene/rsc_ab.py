"""rsc_ab: the scene-record round-trip A/B (S106 G4-1A; docs\\research\\s106\\G4.md "The round-trip proof").

Answers: after a header move, does every STORED record (.rsc, loose or inside a case zip) still parse and re-serialize identically through the
BASE tree's record_io.cpp and this tree's? The same driver (tools\\scene\\rsc_ab_main.cpp) is compiled twice (base tree via `git archive`, this tree);
both exes run over the same record list and print per record: the ParseRecord result, every *_errors counter, every table's row count, the FNV-64
of SerializeRecord(parsed). Any differing line = FAIL with the record's source. No game, no run lock, no SDK; compile only.

  py -I tools\\scene\\rsc_ab.py --base s106 --corpus DIR_OR_FILE [--corpus ...] [--out out\\g4\\rsc_ab] [--limit N]
  py -I tools\\scene\\rsc_ab.py --selftest     (the planted twin: Object grows one uint32 in an overlay copy of this tree's headers -> must FAIL;
                                              the unplanted twin (this tree against itself) must PASS; exit 1 when either arm is wrong)
Exit: 0 = every record identical (or selftest arms right); 1 = a record differs / selftest arm wrong; 2 = could not run (build failed, no records).
Corpus: loose *.rsc under a folder, and `*.rsc` members of *.zip, deduped by sha256. Loose files are read in place (never copied); zip members are
extracted to <out>\\batch in batches and deleted after each.
"""
# entry: rdr_scene::ParseRecord / rdr_scene::SerializeRecord (through tools\scene\rsc_ab_main.cpp)
import argparse, hashlib, os, shutil, subprocess, sys, tarfile, time, zipfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DEFS = ['-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-D_CRT_SECURE_NO_WARNINGS', '-D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING', '-DRDR_DEV']  # tools\scene\build_rsc.ps1:19
BATCH_N, BATCH_BYTES = 1500, 400 * 1024 * 1024


def clang():
    for c in (shutil.which('clang++'),):
        if c:
            return c
    p = Path(r'C:\Program Files\LLVM\bin\clang++.exe')
    if p.exists():
        return str(p)
    r = subprocess.run(['pwsh', '-NoProfile', '-Command', '. tools\\env.ps1 | Out-Null; (Get-Command clang++).Source'], cwd=REPO, capture_output=True, text=True)
    return r.stdout.strip().splitlines()[-1]


def build(tree: Path, out_exe: Path, cxx: str, main_cpp: Path) -> None:
    out_exe.parent.mkdir(parents=True, exist_ok=True)
    cmd = [cxx, '-std=c++20', '-O1', '-fuse-ld=lld', *DEFS, '-I', str(tree / 'src' / 'renderer_scene'), str(main_cpp),
           str(tree / 'src' / 'renderer_scene' / 'record_io.cpp'), '-o', str(out_exe)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        print('BUILD FAILED', out_exe.name, '\n' + (r.stderr or r.stdout)[-3000:])
        sys.exit(2)


def archive_base(base: str, dest: Path) -> None:
    if dest.exists():
        shutil.rmtree(dest, ignore_errors=True)
    dest.mkdir(parents=True)
    tar = dest.parent / 'base.tar'
    subprocess.run(['git', 'archive', '--format=tar', '-o', str(tar), base, 'src'], cwd=REPO, check=True)
    with tarfile.open(tar) as t:
        t.extractall(dest, filter='data')
    tar.unlink()


def sha(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def sha_file(p: Path) -> str:
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def items(corpora):
    """yield (label, path_or_None, bytes_getter). Loose files give a path; zip members give a (zip, member) pair."""
    for c in corpora:
        c = Path(c)
        if c.is_file():
            roots = [c]
        else:
            roots = []
            for dp, _, fns in os.walk(c):
                for fn in fns:
                    if fn.endswith('.rsc') or fn.endswith('.zip'):
                        roots.append(Path(dp) / fn)
        for p in sorted(roots):
            if p.suffix == '.rsc':
                yield (str(p), p, None)
            elif p.suffix == '.zip':
                try:
                    with zipfile.ZipFile(p) as z:
                        names = [n for n in z.namelist() if n.endswith('.rsc') and '/scene/' in ('/' + n)]
                except zipfile.BadZipFile:
                    print('skip bad zip', p)
                    continue
                for n in names:
                    yield (f'{p}!{n}', None, (p, n))


def run_exes(exes, listfile: Path):
    def go(exe):
        r = subprocess.run([str(exe), str(listfile)], capture_output=True, text=True, encoding='utf-8', errors='replace')
        return r.stdout.splitlines(), r.returncode, r.stderr
    with ThreadPoolExecutor(2) as ex:
        return list(ex.map(go, exes))


def compare_batch(a_exe, b_exe, labels, paths, batch_dir: Path, bi: int, stats, bad):
    lf = batch_dir / f'list_{bi}.txt'
    lf.write_text('\n'.join(paths) + '\n', encoding='utf-8')
    (oa, ra, ea), (ob, rb, eb) = run_exes([a_exe, b_exe], lf)
    if ra or rb or len(oa) != len(paths) or len(ob) != len(paths):
        bad.append((f'batch {bi}', f'exe exit {ra}/{rb}, lines {len(oa)}/{len(ob)} of {len(paths)}: {(ea or eb)[:200]}'))
        return
    for lab, x, y in zip(labels, oa, ob):
        stats['n'] += 1
        if x != y:
            bad.append((lab, f'A: {x[:300]}\n    B: {y[:300]}'))
        else:
            stats['ok_parse' if ' ok ' in x else 'err_parse'] += 1
            if ' ok ' not in x:   # S106 G4-1AF: name every record both trees refuse (label + the error text), so a refusal is never silent
                stats.setdefault('refused', []).append((lab, x[:200]))
            stats['same_bytes'] += x.endswith('same=1')
    lf.unlink()


def corpus_run(a_exe, b_exe, corpora, out: Path, limit=0):
    t0 = time.time()
    batch_dir = out / 'batch'
    stats = {'n': 0, 'ok_parse': 0, 'err_parse': 0, 'same_bytes': 0, 'dupes': 0}
    bad = []
    seen = set()
    labels, paths, tmp_files, nbytes, bi = [], [], [], 0, 0
    zips = {}

    def flush():
        nonlocal labels, paths, tmp_files, nbytes, bi
        if paths:
            compare_batch(a_exe, b_exe, labels, paths, batch_dir, bi, stats, bad)
            bi += 1
        for t in tmp_files:
            try:
                t.unlink()
            except OSError:
                pass
        labels, paths, tmp_files, nbytes = [], [], [], 0

    shutil.rmtree(batch_dir, ignore_errors=True)
    batch_dir.mkdir(parents=True, exist_ok=True)
    count = 0
    for label, path, zm in items(corpora):
        if path is not None:
            h = sha_file(path)
            if h in seen:
                stats['dupes'] += 1
                continue
            seen.add(h)
            labels.append(label), paths.append(str(path))
            nbytes += path.stat().st_size
        else:
            zp, name = zm
            z = zips.get(zp)
            if z is None:
                zips.clear()
                z = zips[zp] = zipfile.ZipFile(zp)
            data = z.read(name)
            h = sha(data)
            if h in seen:
                stats['dupes'] += 1
                continue
            seen.add(h)
            f = batch_dir / f'm{len(seen)}.rsc'
            f.write_bytes(data)
            tmp_files.append(f)
            labels.append(label), paths.append(str(f))
            nbytes += len(data)
        count += 1
        if len(paths) >= BATCH_N or nbytes >= BATCH_BYTES:
            flush()
        if limit and count >= limit:
            break
    flush()
    shutil.rmtree(batch_dir, ignore_errors=True)
    return stats, bad, time.time() - t0


def main_run(args):
    out = Path(args.out) if os.path.isabs(args.out) else REPO / args.out
    cxx = clang()
    old = out / 'old'
    t0 = time.time()
    archive_base(args.base, old)
    main_cpp = REPO / 'tools' / 'scene' / 'rsc_ab_main.cpp'
    a_exe, b_exe = out / 'rsc_ab_base.exe', out / 'rsc_ab_new.exe'
    with ThreadPoolExecutor(2) as ex:
        fa = ex.submit(build, old, a_exe, cxx, main_cpp)
        fb = ex.submit(build, REPO, b_exe, cxx, main_cpp)
        fa.result(), fb.result()
    print(f'rsc_ab: built base ({args.base}) and this tree in {time.time() - t0:.0f} s')
    stats, bad, secs = corpus_run(a_exe, b_exe, args.corpus, out, args.limit)
    print(f"rsc_ab: {stats['n']} records compared ({stats['dupes']} duplicates skipped), {stats['n'] - len(bad)} identical, {len(bad)} differ; "
          f"{stats['ok_parse']} parse ok, {stats['err_parse']} refused by both, re-serialized bytes equal the file in {stats['same_bytes']}; {secs:.0f} s")
    for lab, why in bad[:20]:
        print('  DIFF', lab, '\n    ' + why)
    ref = stats.get('refused', [])
    for lab, why in ref[:20]:
        print('  REFUSED by both', lab, '|', why)
    if len(ref) > 20:
        print(f'  ... and {len(ref) - 20} more refused by both')
    if not stats['n']:
        print('rsc_ab: NO RECORDS (check --corpus)')
        return 2
    print('rsc_ab: PASS' if not bad else 'rsc_ab: FAIL')
    return 0 if not bad else 1


def selftest(args):
    out = Path(args.out) if os.path.isabs(args.out) else REPO / args.out
    out = out / 'selftest'
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    cxx = clang()
    main_cpp = REPO / 'tools' / 'scene' / 'rsc_ab_main.cpp'
    # the overlay: only the headers record_io.cpp reads (clang -MM), copied from this tree, then planted
    r = subprocess.run([cxx, '-std=c++20', '-MM', *DEFS, '-I', str(REPO / 'src' / 'renderer_scene'), str(REPO / 'src' / 'renderer_scene' / 'record_io.cpp')],
                       capture_output=True, text=True)
    if r.returncode:
        print('selftest: -MM failed', r.stderr[-500:])
        return 2
    deps = [Path(x) for x in r.stdout.replace('\\\n', ' ').replace('\\\r\n', ' ').split()[1:] if x.strip() and x != '\\']
    ov = out / 'overlay'
    for d in deps:
        d = d if d.is_absolute() else REPO / d
        rel = d.resolve().relative_to(REPO)
        (ov / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(d, ov / rel)
    shutil.copy2(REPO / 'src' / 'renderer_scene' / 'record_io.cpp', ov / 'src' / 'renderer_scene' / 'record_io.cpp')
    sr_candidates = [ov / 'src' / 'renderer_scene' / 'scene_record.h']
    marker = '  float tint[4] = {1, 1, 1, 1};      // fades (object alpha), 1 = none'
    planted = False
    for p in sr_candidates:
        s = p.read_bytes().decode('utf-8')
        if marker in s:
            s = s.replace(marker, marker + '\r\n  uint32_t planted_extra = 0;   // S106 G4 selftest: the planted twin', 1)
            p.write_bytes(s.encode('utf-8'))
            planted = True
    if not planted:
        print('selftest: marker line of Object not found in scene_record.h (the planted twin cannot be made): BLOCKED')
        return 2
    new_exe, plant_exe = out / 'new.exe', out / 'planted.exe'
    with ThreadPoolExecutor(2) as ex:
        fa = ex.submit(build, REPO, new_exe, cxx, main_cpp)
        fb = ex.submit(build, ov, plant_exe, cxx, main_cpp)
        fa.result(), fb.result()
    rec = out / 'synthetic.rsc'
    r = subprocess.run([str(new_exe), '--make', str(rec)])
    if r.returncode or not rec.exists():
        print('selftest: --make failed')
        return 2
    labels, paths = ['synthetic'], [str(rec)]
    bad_ok, bad_plant, st = [], [], {'n': 0, 'ok_parse': 0, 'err_parse': 0, 'same_bytes': 0}
    compare_batch(new_exe, new_exe, labels, paths, out, 0, st, bad_ok)
    parsed_ok = st['ok_parse'] == 1
    compare_batch(new_exe, plant_exe, labels, paths, out, 1, st, bad_plant)
    print(f'selftest: unplanted twin (this tree vs itself): {"PASS" if not bad_ok and parsed_ok else "WRONG"} (synthetic record parsed ok: {parsed_ok})')
    print(f'selftest: planted twin (Object + one uint32): {"FAIL as it must" if bad_plant else "PASSED (WRONG: the tool says green on a layout change)"}')
    if bad_plant:
        print('  ' + bad_plant[0][1].replace('\n', '\n  ')[:400])
    good = (not bad_ok) and parsed_ok and bool(bad_plant)
    print('selftest: ' + ('OK' if good else 'NOT OK'))
    return 0 if good else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--base', default='s106')
    ap.add_argument('--corpus', action='append', default=[])
    ap.add_argument('--out', default=r'out\g4\rsc_ab')
    ap.add_argument('--limit', type=int, default=0, help='stop after N distinct records (development)')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        return selftest(a)
    if not a.corpus:
        ap.error('--corpus is required (or --selftest)')
    return main_run(a)


if __name__ == '__main__':
    sys.exit(main())
